/* ***********************************************************************
 * Andama Proxy Server
 * (C) 2014 by Yiannis Bourkelis (hello@andama.org)
 *
 * This file is part of Andama Proxy Server.
 *
 * Andama Proxy Server is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Andama Proxy Server is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Andama Proxy Server.  If not, see <http://www.gnu.org/licenses/>.
 * ***********************************************************************/

// The C10K problem: http://www.kegel.com/c10k.html
#ifdef WIN32
#define NOMINMAX
#include <stdio.h>
#include "winsock2.h"
#include <ws2tcpip.h>
#define bzero(b,len) (memset((b), '\0', (len)), (void) 0)
#define bcopy(b1,b2,len) (memmove((b2), (b1), (len)), (void) 0)
#pragma comment(lib, "Ws2_32.lib")

#else
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "sys/time.h"
#include <signal.h> //signal(SIGPIPE, SIG_IGN);
#include <netinet/tcp.h> //gia to TCP_NODELAY
#endif

#include <string.h>
#include <sstream>
#include <map>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <iostream> //std::cout
#include <errno.h>
#include <array>
#include <random>
#include <chrono>

/* Elegxoi prin ginei deploy
 * --------------------------
 * - PORT_NUMBER = 443  (https port)
 * - MIN_ID = 100000000 (minimum available random id)
 * - MAX_ID = 999999999 (maximum available random id)
 * - MAX_WRONG_ID_TRIES         = 10   (id sniffing protection)
 * - MAX_WRONG_ID_TRIES_WARNING = 5    (id sniffing protection)
 * - WRONG_ID_BAN_DURATION      = 7200 (id sniffing protection)
 */

//server listening port
const int PORT_NUMBER = 8099;

//to evros tou client id pou dimiourgeitai tyxaia
//gia kathe client
const unsigned long int MIN_ID = 100000; // default 100000000
const unsigned long int MAX_ID = 999999; // default 999999999

//id sniffing protection
const int  MAX_WRONG_ID_TRIES         = 10;  //10; //default: diplasio apo to MAX_WRONG_ID_TRIES_WARNING
const int  MAX_WRONG_ID_TRIES_WARNING = 5;  //5;
const long WRONG_ID_BAN_DURATION      = 7200; //7200 - 2 HOURs

//ta commands pou anagnwrizei o proxy
const std::array<char,1> CMD_PROTOCOL = {{'P'}};
const std::array<char,1> CMD_CONNECT = {{'C'}};
const std::array<char,1> CMD_CONNECT_ID_NOT_FOUND = {{'c'}}; // stelnetai apo ton server otan den vrethei to id pou zitithike apo CMD_CONNECT
//const std::array<char,1> CMD_QUIT = {{'Q'}};
const std::array<char,1> CMD_ACCEPT = {{'A'}};
const std::array<char,1> CMD_ID = {{'I'}};
const std::array<char,1> CMD_SCREENSHOT = {{'S'}};
const std::array<char,1> CMD_SCREENSHOT_DIFF = {{'s'}};
const std::array<char,1> CMD_REQUEST_SCREENSHOT = {{'R'}};
const std::array<char,1> CMD_REQUEST_SCREENSHOT_DIFF = {{'r'}};
const std::array<char,1> CMD_MOUSE = {{'M'}}; // mouse
const std::array<char,1> CMD_KEYBOARD = {{'K'}}; // KEYBOARD
const std::array<char,1> CMD_DISCONNECT_FROM_REMOTE_COMPUTER = {{'D'}}; // disconnect from computer
const std::array<char,1> CMD_HEART_BEAT = {{'H'}};
const std::array<char,1> CMD_BAN_IP_WRONG_ID = {{'B'}}; // synolika 5 bytes
const std::array<char,1> CMD_WARNING_BAN_IP_WRONG_ID= {{'W'}}; // synolika 2 bytes
const std::array<char,1> CMD_CONNECT_PASSWORD_NOT_CORRECT= {{'p'}}; // synolika 1 byte
const std::array<char,1> CMD_BAN_IP_WRONG_PWD = {{'b'}}; //synolika ena byte (mono to command)
const std::array<char,1> CMD_WARNING_BAN_IP_WRONG_PWD= {{'w'}}; // synolika 2 bytes
const std::array<char,1> CMD_ERROR_APP_VERSION_NOT_ACCEPTED = {{'V'}}; // epistrefei to URL pros ti selida pou tha katevasei o xristis tin neoteri ekdosi tis efarmogis
//const std::array<char,1> CMD_ERROR_PROTOCOL_VERSION_NOT_ACCEPTED = {{'v'}}; // (mono command)


//enums
//prosdiorizei ean o client syndeetai ston host mesw tou proxy
//h apeftheias (peer-to-peer)
enum connectMessageType {
    proxy = 0,
    direct = 1
};

//random ID generator
std::random_device rd;
std::mt19937_64 mt(rd());
std::uniform_int_distribution<unsigned long int> distribution(MIN_ID,MAX_ID);


void error(const char *msg) {
    perror(msg);
    exit(1);
}

//gia kathei client pou syndeetai ston server
//kataxaritai sto clients map ena instance tis klasis me plirofories gia ton client
class ClientInfo
{
public:
    int sockfd = -1; //to socket epikoinwnias me ton client
    int remote_socket = -1; //to socket epikoinwnias me ton remote client an ginei accept
    std::string remote_id = ""; //to id to remote client ean exei ginei accept
    std::vector<char> cachedID; //to cachedID pou xrisiopoieitai gia na diatirei o client to idio ID se periptwsi aposyndesis
};

//kathe fora pou ena client zita na syndethei se id
//pou den yparxei, ena instance aftis tis klasis kataxwreitai
//sto protect_id map.
//Me tin voitheia tis ginetai tracking ean to idio ip kanei id sniffing
class IPProtection
{
public:
    std::chrono::high_resolution_clock::time_point lastWrongIDTime; //i stigmi pou egine i teleftaia lathos apopeira
    int wrongIDCounter = 0;//to plithos twn lathos apopeirwn
};

std::mutex clients_mutex;//sygxronismos clients map
std::mutex send_mutex; //sygxronismos sockets.TODO (xreiazetai sygxronismos wste se kamia periptwsi na mi ginetai apo diaforetika thread lipsi i apostoli sto idio socket
std::mutex protect_id_mutex;//sygxronismos proteck_id map
std::map<std::string, ClientInfo> clients;//krataei plirofories gia ton kathe client poy einai syndedemenos sto server
std::map<unsigned long,IPProtection> protect_id;//krataei tis ip pou exoun kanei apopeira syndesis se id pou den yparxei
std::map<std::vector<char>,std::string> cachedIDs;

//random CachedID generator
std::string cachedID_Charset = "1234567890abcdefghjgklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXY!@#$%^&*()_+{}<>,.";
std::random_device rd_cachedID;
std::mt19937 mt_cachedID = std::mt19937(rd_cachedID());
std::uniform_int_distribution<unsigned int> distribution_cachedID = std::uniform_int_distribution<unsigned int>(0, cachedID_Charset.length() - 1);


//console logging
std::string getTime()
{
#ifdef WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string ret;
    ret.insert(ret.begin(),st.wYear);
    return ret;
#else
    struct timeval tv;
    time_t nowtime;
    struct tm *nowtm;
    char tmbuf[64], buf[64];

    gettimeofday(&tv, NULL);
    nowtime = tv.tv_sec;
    nowtm = localtime(&nowtime);
    strftime(tmbuf, sizeof tmbuf, "[%Y-%m-%d %H:%M:%S", nowtm);
    snprintf(buf, sizeof buf, "%s.%06lu]", tmbuf, tv.tv_usec);
    return std::string(buf);
#endif
}

std::vector<char> generateRandomCachedID(int length)
{
    std::string result;
    result.resize(length);

    for (int i = 0; i < length; i++){
        int d = distribution_cachedID(mt_cachedID);
        result[i] = cachedID_Charset[d];
    }

    return std::vector<char> (result.begin(),result.end());
}

//dimiourgei kai epistrefei ena tyxaio kai monadiko clientid
#ifdef WIN32
std::string getClientID(const SOCKET socketfd, std::vector<char> cachedID) {
#else
std::string getClientID(const int socketfd, std::vector<char> cachedID) {
#endif
    std::lock_guard<std::mutex> lock(clients_mutex);
    while(true)
    {
        //ean exei dothei cachedID:
        //elegxw ean yparxei sto map
        //wste na epistrepsw to idio ID pou eixe o client kai prin.
        if (cachedID.size() > 0){
            if (cachedIDs.count(cachedID) > 0)
            {
                //yparxei to cachedID, opote elegxw ean den yparxei o client idi energos
                if (clients.count(cachedIDs.at(cachedID)) == 0){
                    //den yparxei o client energos, ara eimaste OK na
                    //epistrepsoume to yparxon ID pou zitithike

                    //prosthetw ton client sto dictionary me olous tous clients
                    ClientInfo ci;
                    ci.remote_socket = 0;
                    ci.sockfd = socketfd;
                    ci.cachedID = cachedID;
                    clients[cachedIDs.at(cachedID)] = ci;

                    return cachedIDs.at(cachedID);
                }
            }
        }

        //ean 1:den exei dothei cached id h
        //    2:to cachedid pou exei dothei den yparxei h
        //    3:to cachedid pou exei dothei yparxei alla o client einai syndedemenos
        //dimiourgw neo id kai cached id gia ton client pou tha syndethei
        unsigned long int dice_roll = distribution(mt);  // generates number in the range MIN_ID...MAX_ID
        std::stringstream tmpIDstream;
        tmpIDstream << dice_roll;

        if (clients.count(tmpIDstream.str()) == 0)
        {
            //prosthetw ton client sto dictionary me olous tous clients
            ClientInfo ci;
            ci.remote_socket = 0;
            ci.sockfd = socketfd;
            ci.cachedID = generateRandomCachedID(64);
            clients[tmpIDstream.str()] = ci;

            //prostheto to neo cachedID sto map
            cachedIDs[ci.cachedID]=tmpIDstream.str();

            return tmpIDstream.str();
        }
    }
}

//emfanizei to teleftaio sfalma pou proklithike
void displayErrno(std::string source)
{
#ifdef WIN32
    std::cout << getTime() << " " << std::this_thread::get_id() << " " <<
                 "###### [source: " << source <<  "]  displayErrno: " << strerror(WSAGetLastError()) << std::endl;
#else
    std::cout << getTime() << " " << std::this_thread::get_id() << " " <<
                 "###### [source: " << source <<  "]  displayErrno: " << strerror(errno) << std::endl;
#endif
}

//diaxeirisi unhundled exceptions
//http://akrzemi1.wordpress.com/2011/10/05/using-stdterminate/
#ifdef WIN32
void onTerminate()
#else
void onTerminate() noexcept
#endif
{
    std::cout << "onTerminate - UNHANDLED EXCEPTION" << std::endl;

    if(auto exc = std::current_exception() ) {
        // we have an exception
        try{
            std::rethrow_exception( exc ); // throw to recognize the type
        }
        catch(const std::exception& e) {
            std::cout << "onTerminate EXCEPTION: " << e.what() << std::endl;
        }
    }

    std::exit( EXIT_FAILURE );
}

//metatropi (serialization) enos int se vector<char> wste na ginetai
//dynati i metafora tou sto socket.
//to buffer pernaei byref kai analoga me tin arxikopoiisi pou tou exei ginei
//diladei ean exei length 1,2 h 4 bytes ginetai i analogi metatropi
void intToBytes(const int value, std::vector<char> &buffer)
{
    if(buffer.size() == 4){
        buffer[0] = (value >> 24) & 0xff;
        buffer[1] = (value >> 16) & 0xff;
        buffer[2] = (value >> 8)  & 0xff;
        buffer[3] = value & 0xff;
    }
    else if(buffer.size() == 2){
        buffer[0] = (value >> 8)  & 0xff;
        buffer[1] = value & 0xff;
    }
    else if(buffer.size() == 1){
        buffer[0] = value & 0xff;
    }
}

//lamvanei ena vector<char> pou periexei arithmo pou egine
//serialize me to intToBytes kai epistrefei ton arxiko arithmo.
//epitrepetai na exei megethos 1,2 h 4 bytes.
int bytesToInt(const std::vector<char> &buffer)
{
    if (buffer.size() == 4){
        return
            (((unsigned char)buffer[0]) << 24) +
            (((unsigned char)buffer[1]) << 16) +
            (((unsigned char)buffer[2]) << 8) +
              (unsigned char)buffer[3];
    }
    if (buffer.size() == 2){
        return
            (((unsigned char)buffer[0]) << 8) +
              (unsigned char)buffer[1];
    }
    else if (buffer.size() == 1){
        return
              (unsigned char)buffer[0];
    }
    return 0;
}

//ean symvei kapoio sfalma sto thread pou diaxeirizetai to client connection
//i ginei timeout, edw katharizoume kapoia resources pou xrisimopoiountai
#ifdef WIN32
void cleanup(const std::string id, const SOCKET socketfd)
#else
void cleanup(const std::string id, const int socketfd) noexcept
#endif
{
    //-afairei to id apo to clients collection
    //- TODO: psaxnei ollo to clients collection kai ean vrei to id to svinei
    //-den sikwnei exceptiom
    //kleinei to socket
    std::lock_guard<std::mutex> lock(clients_mutex);
    clients.erase(id);
    #ifdef WIN32
    closesocket(socketfd);
    #else
    close(socketfd);
    #endif
}

//apostelei to command(1byte) kai to mynima efoson yparxei
#ifdef WIN32
int _sendmsgPlain(const SOCKET socketfd, const std::array<char, 1> &command, const std::vector<char> &message = std::vector<char>())
#else
int _sendmsgPlain(const int socketfd, const std::array<char, 1> &command, const std::vector<char> &message = std::vector<char>())
#endif
{
    std::vector<char> msg(1);
    msg[0]=command[0];

    if (message.size() > 0){
        msg.insert(msg.begin()+1, message.begin(),message.end());
    }

    std::size_t total = 0;        // how many bytes we've sent
    { // lock_guard scope
        std::lock_guard<std::mutex> lock(send_mutex);

        //prepei na vevaiwthooume oti stalthike olo to mynhma
        size_t bytesleft = msg.size(); // how many we have left to send
        int n;
        while (total < msg.size()){
            n = send(socketfd,msg.data()+total,bytesleft,0);
            if (n < 0){
                break;
            }
            else if (n == 0)
            {
                std::cout << "-----> send returned 0 bytes. Expected: " << msg.size() <<
                             "  [ int clientserver::_sendmsgPlain(int socketfd, const std::array<char, 1> &command,const std::vector<char> &message) ]" << std::endl;
                displayErrno("int clientserver::_sendmsgPlain(int socketfd, const std::array<char, 1> &command,const std::vector<char> &message)");
                return 0;
            }
            total+=n;
            bytesleft-=n;
        }
    } // lock_guard scope

    if (total != msg.size()){
        std::cout << "----> throw EXCEPTION IN int clientserver::_sendmsgPlain(int socketfd, const std::array<char, 1> &command,const std::vector<char> &message). Bytes send not equal to bytes expected. Command: %s" << command[0] << std::endl;
        displayErrno("int clientserver::_sendmsgPlain(int socketfd, const std::array<char, 1> &command,const std::vector<char> &message)");
        throw std::runtime_error(std::string("----> throw EXCEPTION IN int clientserver::_sendmsgPlain(int socketfd, const std::array<char, 1> &command,const std::vector<char> &message). Bytes send not equal to bytes expected."));
    }
    return total;
}


/* apostelei:
 * - to command (1 byte)
 * - ypologizei me 4 byte to megethos tou mynimatos
 * - to mynima
 * - Veltiwseis pou tha mporousan na ginoun:
 * a)anti na antigrafw to message sto msg, na kataxwrw to msg stin arxi tou message
 */
#ifdef WIN32
int _sendmsg(const SOCKET socketfd, const std::array<char, 1> &command, const std::vector<char> &message)
#else
int _sendmsg(const int socketfd,    const std::array<char, 1> &command, const std::vector<char> &message)
#endif
{
    //std::cout << "clientserver::sendmsg called" << std::endl;

    std::vector<char> msg(5);
    int len = message.size();

    //std::cout << "16. Message len: " << len << std::endl;

    std::vector<char> lenb(4);
    intToBytes(len,lenb);

    msg[0]=command[0];
    msg[1]=lenb[0];
    msg[2]=lenb[1];
    msg[3]=lenb[2];
    msg[4]=lenb[3];

    msg.insert(msg.begin()+5, message.begin(),message.end());

    std::size_t total = 0;        // how many bytes we've sent
    { // lock_guard scope
        //std::cout << "17. Will lock send socket (sendmutex)" << std::endl;
        std::lock_guard<std::mutex> lock (send_mutex);
        //send_mutex.lock();
        //std::cout << "18. Will send Command: " << &msg.at(0) <<  ". Total bytes: " << msg.size() << std::endl;

        //prepei na vevaiwthooume oti stalthike olo to mynhma
        size_t bytesleft = msg.size(); // how many we have left to send
        int n;
        while (total < msg.size()){
            n = send(socketfd,msg.data()+total,bytesleft,0);
            if (n < 0){
                break;
            }
            else if (n == 0)
            {
                std::cout << "-----> send returned 0 bytes. Expected: " << msg.size() <<
                             "  [ int clientserver::_sendmsg(int socketfd,    const std::array<char, 1> &command, const std::vector<char> &message) ]" << std::endl;
                displayErrno("int clientserver::_sendmsg(int socketfd,    const std::array<char, 1> &command, const std::vector<char> &message)");
                return 0;
            }
            total+=n;
            bytesleft-=n;
        }

        //std::cout << "19. Command: " << &msg[0] << " send. Total bytes returned from socket: " << total << std::endl;
    } // lock_guard scope
    //std::cout << "############ 19. send socket unlocked (sendmutex). END" << std::endl;

    if (total != msg.size()){
        std::cout << "----> throw EXCEPTION IN size_t int clientserver::_sendmsg(int socketfd,    const std::array<char, 1> &command, const std::vector<char> &message). Bytes send not equal to bytes expected. Command: %s" << command[0] << std::endl;
        throw std::runtime_error(std::string("----> throw EXCEPTION IN size_t int clientserver::_sendmsg(int socketfd,    const std::array<char, 1> &command, const std::vector<char> &message). Bytes send not equal to bytes expected."));
    }
    return total;
}

//lamvanei bytes isa me to length tou charbuffer pou pernaei byref
#ifdef WIN32
int _receivePlain(const SOCKET socketfd, std::vector<char> &charbuffer)
#else
int _receivePlain(const int socketfd, std::vector<char> &charbuffer)
#endif
{
    size_t bytes_cnt_payload=0;

    while(bytes_cnt_payload < charbuffer.size())
    {
        int bytes_rcv_payload = recv(socketfd,charbuffer.data() + bytes_cnt_payload,charbuffer.size() - bytes_cnt_payload, 0);

        if (bytes_rcv_payload == 0){
            std::cout << std::this_thread::get_id() << " " <<
                       "####  int recieve: recv return 0 bytes. [int _receivePlain(int socketfd, std::vector<char> &charbuffer)]. Returning from function." << std::endl;
            return 0;
        }
        else if (bytes_rcv_payload == -1){
            displayErrno("int _receivePlain(int socketfd, std::vector<char> &charbuffer)");
            return -1;
        }

        bytes_cnt_payload += bytes_rcv_payload;
    }

    if (bytes_cnt_payload != charbuffer.size()){
        std::cout << "----> EXCEPTION IN recieve func int _receivePlain(int socketfd, std::vector<char> &charbuffer). Bytes recieved not equal to bytes expected. Expected: " << charbuffer.size() << " recieved: " << bytes_cnt_payload << std::endl;
        displayErrno("int clientserver::_sendmsg(int socketfd,    const std::array<char, 1> &command, const std::vector<char> &message)");
        throw std::runtime_error(std::string("----> EXCEPTION IN recieve func (payload). Bytes recieved not equal to bytes expected.\n"));
    }

    return bytes_cnt_payload;
}

/* kanei de-serialize ta prwta 4 bytes tou minimatos
 * pou perigrafoun to megethos tou mynimatos pou akolouthei
 * kai sti synexei lamvanoun to mynima.
 * Afto to mynima to fortwnei sto charbuffer byref
 */
#ifdef WIN32
int _receive(const SOCKET socketfd, std::vector<char> &charbuffer)
#else
int _receive(const int socketfd, std::vector<char> &charbuffer)
#endif
{
    //-----------
    //prwto meros: lamvano to synolo twn bytes poy anamenontai
    //sto payload
    size_t bytes_needed = 4;
    std::vector<char> len_bytes(4);
    size_t bytes_cnt=0;

    while(bytes_cnt < bytes_needed)
    {
        int bytes_rcv = recv(socketfd, len_bytes.data() + bytes_cnt, bytes_needed - bytes_cnt, 0);

        if (bytes_rcv == 0){
            std::cout << std::this_thread::get_id() << " " <<
                       "####  int recieve: recv return 0 bytes. [first while loop]. Returning from function." << std::endl;
            return 0;
        }
        else if (bytes_rcv == -1){
            displayErrno("int _receive(int socketfd, std::vector<char> &charbuffer) - first while loop");
            return -1;
        }

        bytes_cnt += bytes_rcv;
    }

    if (bytes_needed != bytes_cnt){
        std::cout << "----> EXCEPTION IN recieve func (payload length). Bytes recieved not equal to bytes expected. Expected: " << bytes_needed << " recieved: " << bytes_cnt << std::endl;
        displayErrno("int _receive(int socketfd, std::vector<char> &charbuffer) - first while loop");
        throw std::runtime_error(std::string("----> EXCEPTION IN recieve func (payload length). Bytes recieved not equal to bytes expected."));
    }

    //-----------

    //deftero meros: lamvanw to payload
    //std::cout << "_recieve will call bytesToInt to compute required msg size" << std::endl;
    size_t bytes_needed_payload = bytesToInt(len_bytes);
    //std::cout << "_recieve required msg size: " << bytes_needed_payload << " >> charbuffer.max_size():" << charbuffer.max_size() << " Will resize charbuffer..." << std::endl;
    //SIMANTIKO!!! An syndethw me telnet kai kataxwrisw P010 o server krasarei se linux giati
    //apo oti katalavainw den mporei na kanei resize to charbuffer se ena poly megalo arithmo.
    //Gi afto kanw ton elegxo parakatw pou tha prostatevei kai ton server se periptwsi pou kapoios paei na
    //steilei p.x. panw apo 20mb dedomenwn gia na tou dimiourgisei provlima.
    if (bytes_needed_payload < 20971520){ //maximum peripou 20 MB gia prostasia
        charbuffer.resize(bytes_needed_payload);
    }
    else {
        std::cout << "Error on _receive: cannot receive more than 20971520 bytes at once" << std::endl;
        throw std::runtime_error("_receive > cannot receive more than 20971520 bytes at once");
    }
    size_t bytes_cnt_payload = 0;

    while(bytes_cnt_payload < bytes_needed_payload)
    {
        int bytes_rcv_payload = recv(socketfd,charbuffer.data() + bytes_cnt_payload,bytes_needed_payload - bytes_cnt_payload, 0);

        if (bytes_rcv_payload == 0){
            std::cout << std::this_thread::get_id() << " " <<
                       "####  int recieve: recv return 0 bytes. [second while loop]. Returning from function." << std::endl;
            return 0;
        }
        else if (bytes_rcv_payload == -1){
            displayErrno("int _receive(int socketfd, std::vector<char> &charbuffer) - second while loop");
            return -1;
        }

        bytes_cnt_payload += bytes_rcv_payload;
    }

    if (bytes_cnt_payload != bytes_needed_payload){
        std::cout << "----> EXCEPTION IN recieve func (payload). Bytes recieved not equal to bytes expected. Expected: " << bytes_needed_payload << " recieved: " << bytes_cnt_payload << std::endl;
        displayErrno("int _receive(int socketfd, std::vector<char> &charbuffer) - second while loop");
        throw std::runtime_error(std::string("----> EXCEPTION IN recieve func (payload). Bytes recieved not equal to bytes expected."));
    }

    return bytes_cnt_payload + bytes_cnt;
}

//epistrefei to socket tou remote computer poy einai syndedemeno ston client me to id eisodou
int getRemoteComputerSocket(const std::string &ID)
{
    std::lock_guard<std::mutex> lock (clients_mutex);
    return clients[ID].remote_socket;
}

//epistrefei to id tou remote computer poy einai syndedemeno ston client me to id eisodou
std::string getRemoteComputerID(const std::string &ID)
{
    std::lock_guard<std::mutex> lock (clients_mutex);
    return clients[ID].remote_id;
}

#ifdef WIN32
bool isBannedForWrongIDs(const SOCKET socketfd, const unsigned long clientIP)
#else
bool isBannedForWrongIDs(const int socketfd, const in_addr_t clientIP)
#endif
{
    std::lock_guard<std::mutex> lock(protect_id_mutex);

    if (protect_id.count(clientIP) == 0){
        //den yparxei kataxwrisi gia afti tin ip opote
        //epistrefw false (oxi ban)
        return false;
    }

    //yparxei kataxwrisi tis IP opote elegxw ean exei ginei ban
    IPProtection & ipp = protect_id[clientIP];

    std::chrono::high_resolution_clock::time_point curr_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> reamainingBanTime = std::chrono::duration_cast<std::chrono::duration<double>>(curr_time - ipp.lastWrongIDTime);
    std::chrono::seconds s(WRONG_ID_BAN_DURATION);

    if (reamainingBanTime.count() > WRONG_ID_BAN_DURATION){
        //exei perasei perisoteros xronos apo to megisto gia lathos ID
        //opote kanw reset to ID counter.
        //Ean exei ginei ban tis IP, edw to ban katargeitai kai ksanametran ola apo tin arxh
        ipp.wrongIDCounter = 0;
        return false;
    }
    else if (ipp.wrongIDCounter >= MAX_WRONG_ID_TRIES){
        // *** BAN ***
        //kseperase to megisto orio lathos prospathiwn
        //opote kanw ban tin ip kai
        //enimerwnw ton client oti exei ginei ban
        //stelnwntas gia posa seconds akoma tha diarkei to ban
        reamainingBanTime = s - reamainingBanTime;
        std::vector<char> sc(4);
        intToBytes(reamainingBanTime.count(), sc);
        _sendmsgPlain(socketfd,CMD_BAN_IP_WRONG_ID,sc);
        return true;
    }

    return false; // oxi ban
}

//otan to remote ID pou zitithike den yparxei
//kataxwreitai to IP wste na prostatevetai o server
//apo ID sniffing.
//Epistrefei true ean i IP epitrepetai na syndethei sto server
//kai false ean i IP einai Banned
#ifdef WIN32
bool add_wrong_ID(const SOCKET socketfd, const unsigned long clientIP)
#else
bool add_wrong_ID(const int socketfd, const in_addr_t clientIP)
#endif
{
    std::lock_guard<std::mutex> lock(protect_id_mutex);

    if (protect_id.count(clientIP) == 0){
        //den vrethike to clientIP
        //opote kataxwrw tin ip pou ekane apotiximeni prospatheia
        //evresis id kai arxikopoiw tis times
        IPProtection ipp;
        ipp.lastWrongIDTime =  std::chrono::high_resolution_clock::now();
        ipp.wrongIDCounter = 1;
        protect_id[clientIP] = ipp;

        //enimerwnw kai ton client pou ekane to aitima
        _sendmsgPlain(socketfd,CMD_CONNECT_ID_NOT_FOUND);

        return true;
    }
    else {
        //vrethike clientIP.
        //elegxw ean exei perasei o megistos xronos gia lathos ID h oxi
        IPProtection & ipp = protect_id[clientIP];
        std::chrono::high_resolution_clock::time_point curr_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(curr_time - ipp.lastWrongIDTime);
        if (time_span.count() > WRONG_ID_BAN_DURATION){
            //exei perasei perisoteros xronos apo to megisto gia lathos ID
            //opote kanw reset to ID counter.
            //Ean exei ginei ban tis IP, edw to ban katargeitai kai ksanametran ola apo tin arxh
            ipp.wrongIDCounter = 1;
            ipp.lastWrongIDTime = curr_time;

            //enimerwnw kai ton client pou ekane to aitima
            _sendmsgPlain(socketfd,CMD_CONNECT_ID_NOT_FOUND);

            return true;
        }
        else {
            //epanalamvanomena lathos ID mesa sto xrono gia ban IP
            //Elegxw ean prepei na ginei ban h oxi akoma
            ipp.wrongIDCounter++;
            if (ipp.wrongIDCounter <= MAX_WRONG_ID_TRIES){
                //efoson akoma den exei ginei ban, thetw ton xrono tis
                //teleftaias apotyximenis prospatheias.
                //Otan ginei ban den xreiazetai na thesw ton xrono wste
                //na mporei na metraei antistrofa mexri na eksantlithei o xronos tou ban.
                ipp.lastWrongIDTime = curr_time;
            }

            //Elegxos ean prepei na ginei ban tis IP
            if (ipp.wrongIDCounter >= MAX_WRONG_ID_TRIES){
                // *** BAN ***
                //kseperase to megisto orio lathos prospathiwn
                //opote kanw ban tin ip kai
                //enimerwnw ton client oti exei ginei ban
                //stelnwntas gia posa seconds akoma tha diarkei to ban
                std::chrono::duration<double> reamainingBanTime = std::chrono::duration_cast<std::chrono::duration<double>>(curr_time - ipp.lastWrongIDTime);
                std::chrono::seconds s(WRONG_ID_BAN_DURATION);
                reamainingBanTime = s - reamainingBanTime;
                std::vector<char> sc(4);
                intToBytes(reamainingBanTime.count(), sc);
                _sendmsgPlain(socketfd,CMD_BAN_IP_WRONG_ID,sc);
                return false;
            }
            else {
                //den exei ginei ban akoma opote
                //eksetazw ean prepei na apostalei warning h oxi
                if (ipp.wrongIDCounter >= MAX_WRONG_ID_TRIES_WARNING){
                    //apostoli warning
                    int remain = MAX_WRONG_ID_TRIES - ipp.wrongIDCounter;
                    std::vector<char> vremain(1);
                    intToBytes(remain, vremain);
                    _sendmsgPlain(socketfd,CMD_WARNING_BAN_IP_WRONG_ID,vremain);
                    return true;
                }
                else {
                    //den exoume ftasei akomi sta lathi pou
                    //prepei na emfanistei warning opote
                    //ola ok

                    //enimerwnw kai ton client pou ekane to aitima
                    _sendmsgPlain(socketfd,CMD_CONNECT_ID_NOT_FOUND);

                    return true;
                }
            }
        }
    }
}

/* Gia na syndethei enas ypologistis se ena allo, prepei na steilei to ID tou wste
 * na kserei o allos poios einai (xrisimo se periptwsi poy ylopoiithei allagi plevras)
 * kai to password tou allou ypologisti wste na ginei authentication.
 * H morfi tou command exei ws ekshs:
 * | 1 byte command | 4 byte msg payload | 1 byte pou prosdiorizei to ID length_1 | ID me bytes to length_1 | password me bytes oso payload - length_1 |
 */

void createConnectCommandData(std::vector<char> &all_data, const std::vector<char> &remoteComputerID, const std::vector<char> &remoteComputerPassword, const unsigned long clientIP, int os)
{
    //1 byte msg type
    std::vector<char> connMsgTypeSize(1);
    intToBytes(connectMessageType::proxy, connMsgTypeSize);
    all_data.insert(all_data.begin(),connMsgTypeSize[0]); // size tou connMsgTypeSize

    //1 byte OS
    std::vector<char> connOSSize(1);
    intToBytes(os, connOSSize);
    all_data.insert(all_data.end(),connOSSize[0]); // to trexon leitourgiko


    //ypologismos ID size kai kataxwrisi ID
    std::vector<char> idSize(1);
    intToBytes(remoteComputerID.size(),idSize);
    all_data.insert(all_data.end(), idSize[0]); // size tou ID
    all_data.insert(all_data.end(), remoteComputerID.begin(), remoteComputerID.end()); // to ID

    //ypologismos PWD size kai kataxwrisi PWD
    std::vector<char> pwdSize(1);
    intToBytes(remoteComputerPassword.size(),pwdSize);
    all_data.insert(all_data.end(), pwdSize[0]); // size tou PWD
    all_data.insert(all_data.end(), remoteComputerPassword.begin(), remoteComputerPassword.end()); // to PWD

    //eisagwgi tis IP sto telos
    std::vector<char> IPbytes(4);
    intToBytes(clientIP,IPbytes);
    all_data.insert(all_data.end(),IPbytes.begin(),IPbytes.end());
}

/*
 * Sep 2014
 *
 * Edw einai to loop pou lamvanontai gia kathe client socket oi entoles.
 *
 * H filosofia oson afora ta sfalmata einai oti to thread DEN katastrefetai
 * ean apotyxei apostoli dedomenwn pros trito socket. (<<< DEN EXEI YLOPOIITHEI ETSI. NA TO DW)
 *
 * Episis xreiazetai idietairi prosoxi sta exceptions giati ean xtypisei kapou
 * kai den diaxeiristei to exception, mazi me to thread peftei kai o olokliros o server.
 *
 * PROFYLAKSEIS
 * - todo: apo tin idia ip mporoun na syndethoun to megisto 50 clients
 * - todo: i idia ip mporei na steilei to megisto 10 request pros client id pou den yparxei. meta ginetai ban gia 2 wres. (nomizw to ylopoiisa afto)
 * - todo: flood protect. megisto orio ana client:1GB/hour. megisto orio ana ip:10GB/hour
 */
#ifdef WIN32
void dostuff(const SOCKET socketfd, const unsigned long clientIP) {
#else
void dostuff(const int socketfd, const in_addr_t clientIP) {
#endif
    //std::cout << getTime() << " " << std::this_thread::get_id() <<
    //             " Eisodos sto dostuff loop. Socket:%i" << socketfd  << std::endl;

    std::string myID = ""; // to trexon id tou client pou epikoinwnei me ton server se afto to thread
    std::vector<char>myIDv; // to idio me prin alla ws vector

    //efoson:
    //- to protocolo tou client einai apodekto
    //- o client exei lavei ID
    //- i IP tou client den einei banned - TODO
    //to handshakeCompleted ginetai true.
    //xrisimopoieitai ws ena epipleon epipedo asfaleias
    //wste na min mporei o client na steilei dedomena ston server
    //xwris kapoiou eidous eksousiodotisi
    bool handshakeCompleted = false;

    std::array<char,1> cmdbuffer; // o buffer pou lamvanei to trexon command kathe fora
    int cmd_bytes_recv; // ta bytes pou lifthikan gia to command. Prepei na einai 1.

    try
    {
        //molis syndethei o client, o server apostelei to
        //megisto protocol version poy ypostirizei.
        std::vector<char> server_protocol(2);
        server_protocol[0] = '1';
        server_protocol[1] = '0';
        _sendmsgPlain(socketfd,CMD_PROTOCOL,server_protocol);
    }
    catch (std::exception ex)
    {
        std::cout << "----------> EXCEPTION ********  Synevh exception stin arxi [CMD_PROTOCOL], sto dostuff while loop: " << ex.what() << std::endl;
        displayErrno("----------> EXCEPTION [CMD_PROTOCOL] dostuff loop");
        return;
    }


    //ksekinw tin epesksergasia twn commands pou
    //endexetai na stelnei o client
    while (true) {

        try {
            //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
            //             " 1. Anamoni command..." << std::endl;


            // Kyrio simeio. Lamvanw to command (1 byte)
            cmd_bytes_recv = recv(socketfd, &cmdbuffer[0], 1, 0);
            //std::cout << "DOSTUFF BYTES RECIEVED: " << cmd_bytes_recv << std::endl;

            if (cmd_bytes_recv == 0){
                std::cout << getTime() << " " << std::this_thread::get_id() << " " <<
                             "######### --- Main command loop disconnected from client. ---- ########" << " " <<
                           "####  recv return 0 bytes. [MAIN command loop]. Returning from function." << std::endl;
                cleanup(myID, socketfd);
                return;
            }
            else if (cmd_bytes_recv == -1){
                //edw erxetai kai otan simvei timeout
                std::cout << "----------> Client heartbeat timeout" << std::endl;
                displayErrno("void dostuff(int newsockfd) - [MAIN command loop] - [cmd_bytes_recv == -1]");
                cleanup(myID, socketfd);
                return;
            }

            //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
            //             " 2. Ellava bytes: " << cmd_bytes_recv << ", command: " << &cmdbuffer[0] <<  " Ksekinw epeksergasia tou command" << std::endl;


            if (cmdbuffer == CMD_PROTOCOL) {
                //o client stelnei to app version kai to protocol version pou ypostirizei
                //sti morfi "P<app_major><app_minor><app_revision><protocol_major><protocol_minor><protocol_revision>" opou major,minor,revision apo ena byte.
                //P<app_major><app_minor><app_revision><protocol_major><protocol_minor><protocol_revision><cachedID length 1 bytes> <cachedID>
                //synolo P command 6 bytes <<<<<

                //-- std::vector<char> client_protocolbuff(6);
                //-- _receivePlain(socketfd, client_protocolbuff); //<-----------------------------

                std::cout << "Will recieve protocol info" << std::endl;
                std::vector<char> client_protocolbuff;
                _receive(socketfd, client_protocolbuff); //<-----------------------------
                std::cout << "examining client protocol" << std::endl;
                std::string vapp_maj(client_protocolbuff.begin(),    client_protocolbuff.begin()+1);
                std::string vapp_min(client_protocolbuff.begin() +1, client_protocolbuff.begin()+2);
                std::string vapp_rev(client_protocolbuff.begin() +2, client_protocolbuff.begin()+3);

                std::string vprot_maj(client_protocolbuff.begin()+3, client_protocolbuff.begin()+4);
                std::string vprot_min(client_protocolbuff.begin()+4, client_protocolbuff.begin()+5);
                std::string vprot_rev(client_protocolbuff.begin()+5, client_protocolbuff.begin()+6);
                std::cout << "examining client protocol. app version and protocol version ok" << std::endl;
                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //             " Client protocol: v" << v_maj << "." << v_min << std::endl;

                //edw elegxw ean to client app version kai protocol version einai apodekta
                //prin dimiourgisw new id gia ton client
                //elegxw prota to app version
                if (vapp_maj != "0" || vapp_min != "1"){
                    //mi apodekto version efarmogis.
                    //enimerwnw ton xristi na kanei update
                    std::string app_update_url = "http://forum.andama.org";
                    std::vector<char> vapp_update_url(app_update_url.begin(),app_update_url.end());
                    _sendmsg(socketfd, CMD_ERROR_APP_VERSION_NOT_ACCEPTED,vapp_update_url);
                    return;
                }

                //
                std::vector<char> cachedIDSize(client_protocolbuff.begin() + 6,client_protocolbuff.begin() + 7);
                int icachedIDSize = bytesToInt(cachedIDSize);
                //efou gnwrizoume to length tou id, lamvanoume to id sto vector
                std::vector<char> cachedID(client_protocolbuff.begin() + 7,client_protocolbuff.begin() + 7 + icachedIDSize);


                //dimiourgw id kai ton kataxwrw sto dictionary map
                myID = getClientID(socketfd, cachedID);
                myIDv.insert(myIDv.end(),myID.begin(),myID.end());//to id

                std::vector<char> buffsendID; //to synoliko minima pou tha stalei

                //kataxwrw to megethos tou ID kai to ID
                std::vector<char> buffLenID(1);
                intToBytes(myID.length(),buffLenID);
                buffsendID.insert(buffsendID.end(), buffLenID.begin(),buffLenID.end());
                buffsendID.insert(buffsendID.end(), myID.begin(),myID.end());

                //kataxwrw to megethos to cachedID kai to cachedID
                std::vector<char> buffLenCachedID(1);
                intToBytes(clients[myID].cachedID.size(),buffLenCachedID);
                buffsendID.insert(buffsendID.end(), buffLenCachedID.begin(),buffLenCachedID.end());
                buffsendID.insert(buffsendID.end(), clients[myID].cachedID.begin(),clients[myID].cachedID.end());


                //Stelnw to ID kai to cachedID ston client
                _sendmsg(socketfd, CMD_ID, buffsendID); //>-------------------------

                //prosthetw ton client sto dictionary me olous tous clients
               // { // lock_guard scope
               //     std::lock_guard<std::mutex> lock (clients_mutex);
               //     ClientInfo ci;
               //     ci.remote_socket = 0;
               //     ci.sockfd = socketfd;
               //     clients[myID] = ci;
               // } // lock_guard scope

                handshakeCompleted = true;

                std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                             " CMD_PROTOCOL >  Client ID created and sent to client." << std::endl;

            } // CMD_PROTOCOL

            //proothisi arxikou screenshot ston paralipti
            else if (cmdbuffer == CMD_SCREENSHOT && handshakeCompleted) {
                std::vector<char> screenshot_data_buff;
                _receive(socketfd, screenshot_data_buff);

                _sendmsg(getRemoteComputerSocket(myID), CMD_SCREENSHOT,
                        screenshot_data_buff);//>--------------
            } // CMD_SCREENSHOT

            //proothisi screenshot diff ston paralipti
            else if (cmdbuffer == CMD_SCREENSHOT_DIFF && handshakeCompleted) {
                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //                 " 6.0. CMD_SCREENSHOT_DIFF > Tha ginei recieve twn dedomenwn eikonastou screenshot diff." << std::endl;
                std::vector<char> screenshot_diff_data_buff;
                _receive(socketfd,screenshot_diff_data_buff);

                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //             " 6.1. CMD_SCREENSHOT_DIFF > lifthike. Bytes: " << bytes_recieved << std::endl;

                _sendmsg(getRemoteComputerSocket(myID), CMD_SCREENSHOT_DIFF,
                        screenshot_diff_data_buff);//>--------------
                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //             " 7. CMD_SCREENSHOT_DIFF > To screenshot diff proothithike pros: " << getRemoteComputerID(myID) << " Bytes send: " << bytes_sent << std::endl;
            } // CMD_SCREENSHOT_DIFF

            //proothisi aitimatos gia syndesi se apomakrismeno ypologisti
            else if (cmdbuffer == CMD_CONNECT && handshakeCompleted) {
                std::vector<char> remote_client_idbuff;
                _receive(socketfd,remote_client_idbuff); //<-----------to remote client id pou zitithike kai to remote password

                //ean exei ginei idi ban stin IP den kanw tipota allo
                if(isBannedForWrongIDs(socketfd,clientIP)){
                    continue;
                }

                //agnoow to 1o byte kathws einai to msgtype


                //to 2o byte einai to os
                std::vector<char> connRemoteComputerOS(remote_client_idbuff.begin() + 1, remote_client_idbuff.begin() + 2);
                int os = bytesToInt(connRemoteComputerOS);

                //lamvanw to 3o byte poy mas deixnei to size tou ID
                std::vector<char> idSize(remote_client_idbuff.begin() + 2,remote_client_idbuff.begin() + 3);
                int iIDSize = bytesToInt(idSize);
                //efou gnwrizoume to length tou id, lamvanoume to id sto vector
                std::vector<char> vremoteID(remote_client_idbuff.begin() + 3,remote_client_idbuff.begin() + 3 + iIDSize);

                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //             " CMD_CONNECT > Zitithike syndesi pros client me ID: " << &remote_client_idbuff[0] << std::endl;

                std::lock_guard<std::mutex> lock (clients_mutex);

                //anazitisi ean to remote client id einai kataxwrimeno sto dictionary
                std::string sid(vremoteID.begin(),vremoteID.end());
                std::map<std::string, ClientInfo>::iterator found_client =
                        clients.find(sid);
                if (found_client != clients.end()) {
                    //to remote id pou zitithike vrethike.

                    //lamvanw to size tou password
                    std::vector<char> pwdSize(remote_client_idbuff.begin() + 3 + iIDSize, remote_client_idbuff.begin() + 3 + iIDSize + 1);
                    int ipwdSize = bytesToInt(pwdSize);

                    //lamvanw to password apo to vector poy ellava prin
                    std::vector<char> vpassword(remote_client_idbuff.begin() + 3 + iIDSize + 1, remote_client_idbuff.begin() + 3 + iIDSize + 1 + ipwdSize);

                    std::vector<char> connect_data;
                    createConnectCommandData(connect_data,myIDv,vpassword,clientIP,os);


                    //stelnw sto remote clientid pou zitithike request gia syndesi
                    //kai perimenw tin apantisi
                    clients[sid].remote_socket = socketfd;//theto sto remote client to socket gia epikoinwnia me to client pou zitise tin epikoinwnia
                    _sendmsg(clients[sid].sockfd, CMD_CONNECT, connect_data);//>---------stelnw sto remote client connect request me to client id pou to zitise
                    //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                    //             " CMD_CONNECT > To remote client id: [" << &remote_client_idbuff[0] << "] verithike kai stalthike CMD_CONNECT." << std::endl;
                } else {
                    //to remote id pou zitithike den vrethike
                    //opote enimerwnw to id protection
                    //gia to gegonos oti o client anazitise id pou den yparxei (id sniffing protection)
                    add_wrong_ID(socketfd,clientIP);
                }

            } // CMD_CONNECT

            else if (cmdbuffer == CMD_CONNECT_PASSWORD_NOT_CORRECT && handshakeCompleted) {
                _sendmsgPlain(getRemoteComputerSocket(myID), CMD_CONNECT_PASSWORD_NOT_CORRECT);//>--------------
            } // CMD_CONNECT_PASSWORD_NOT_CORRECT

            else if (cmdbuffer == CMD_BAN_IP_WRONG_PWD && handshakeCompleted) {
                _sendmsgPlain(getRemoteComputerSocket(myID), CMD_BAN_IP_WRONG_PWD);//>--------------
            } // CMD_BAN_IP_WRONG_PWD

            else if (cmdbuffer == CMD_WARNING_BAN_IP_WRONG_PWD && handshakeCompleted) {
                std::vector<char> remain_tries_buff(1);
                _receivePlain(socketfd, remain_tries_buff);
                _sendmsgPlain(getRemoteComputerSocket(myID), CMD_WARNING_BAN_IP_WRONG_PWD, remain_tries_buff);//>--------------
            } // CMD_WARNING_BAN_IP_WRONG_PWD


            //proothisi apodoxis syndesis
            else if (cmdbuffer == CMD_ACCEPT && handshakeCompleted){
                //o client apantise thetika sto aitima gia syndesi
                //stelnontas to remote id pou prokalese to connect
                std::vector<char> accepted_client_id_buff;
                _receive(socketfd,accepted_client_id_buff); // <--------to remote id pou egine accept

                std::lock_guard<std::mutex> lock (clients_mutex);

                //anazitisi ean to remote id pou egine accept yparxei sto dictionary
                std::string sid(accepted_client_id_buff.begin(),accepted_client_id_buff.end());
                std::map<std::string, ClientInfo>::iterator found_client =
                        clients.find(sid);
                if (found_client != clients.end()) {
                    { // lock_guard scope
                        clients[sid].remote_socket = socketfd;
                        clients[sid].remote_id = myID;
                        clients[myID].remote_id = sid;
                    } // lock_guard scope

                    //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                    //             " CMD_ACCEPT > Egine apodoxi apo ton client. Enimerwnw ton kalounta me id: " << &accepted_client_id_buff[0] << std::endl;

                    _sendmsg(clients[sid].sockfd, CMD_ACCEPT, myIDv); //TODO: <edw na stelnw to id aftounou pou ekane accept
                }
                else {
                     std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                                 " CMD_ACCEPT > Stalthike accept apo ton client alla o paraliptis client den vrethike: " << &accepted_client_id_buff[0] << std::endl;
                }
            } // CMD_ACCEPT

            //proothisi aitimatos aposyndesis
            else if (cmdbuffer == CMD_DISCONNECT_FROM_REMOTE_COMPUTER && handshakeCompleted) {
                _sendmsgPlain(getRemoteComputerSocket(myID), CMD_DISCONNECT_FROM_REMOTE_COMPUTER);//>--------------

                std::lock_guard<std::mutex> lock (clients_mutex);

                clients[myID].remote_socket = -1;

                clients[clients[myID].remote_id].remote_socket = -1;
                clients[clients[myID].remote_id].remote_id= "";

                clients[myID].remote_id = "";

            } // CMD_DISCONNECT_FROM_REMOTE_COMPUTER

            //proothisi aitimatos apostolis arxikou screenshot
            else if (cmdbuffer == CMD_REQUEST_SCREENSHOT && handshakeCompleted) {
                _sendmsgPlain(getRemoteComputerSocket(myID), CMD_REQUEST_SCREENSHOT);//>--------------
            } // CMD_REQUEST_SREENSHOT

            //proothisi aitimatos apostolis screenshot diff (diladi mono tis diafores apo to proigoumeno screenshot)
            else if (cmdbuffer == CMD_REQUEST_SCREENSHOT_DIFF && handshakeCompleted) {
                std::vector<char> screenshot_diff_id_data_buff;
                _receive(socketfd, screenshot_diff_id_data_buff);

                std::string rid(screenshot_diff_id_data_buff.begin(),screenshot_diff_id_data_buff.end());

                std::string remoteID = getRemoteComputerID(myID);

                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //            " 4. CMD_REQUEST_SCREENSHOT_DIFF ID: " << rid << ". Tha ginei proothisi pros: " << remoteID << std::endl;

                _sendmsg(getRemoteComputerSocket(myID), CMD_REQUEST_SCREENSHOT_DIFF,screenshot_diff_id_data_buff);//>--------------

                //std::cout << getTime() << " " << std::this_thread::get_id() << " " << myID <<
                //            " 5. CMD_REQUEST_SCREENSHOT_DIFF ID: " << rid  << " > Proothithike pros: " << remoteID << std::endl;
            } // CMD_REQUEST_SREENSHOT_DIFF

            //proothisi mouse
            else if(cmdbuffer == CMD_MOUSE && handshakeCompleted)
            {
                std::vector<char> mouse_data_buff(10);
                _receivePlain(socketfd, mouse_data_buff);
                int remso = getRemoteComputerSocket(myID);
                if (remso > 0)
                {
                    _sendmsgPlain(remso, CMD_MOUSE,mouse_data_buff);//>--------------
                }
            } // CMD_MOUSE

            //proothisi keyboard
            else if(cmdbuffer == CMD_KEYBOARD && handshakeCompleted)
            {
                std::vector<char> keyboard_data_buff(6);
                _receivePlain(socketfd, keyboard_data_buff);
                int remso = getRemoteComputerSocket(myID);
                if (remso > 0)
                {
                    _sendmsgPlain(remso, CMD_KEYBOARD,keyboard_data_buff);//>--------------
                }
            } // CMD_KEYBOARD

            //hearbeat. Ean den lifthei sto xrono tou socket timeout
            //tote diakoptetai i syndesi me ton server
            else if (cmdbuffer == CMD_HEART_BEAT && handshakeCompleted) {
                //den xreiazetai na kanw kati. apla diatirei ton client syndedemeno
                //kathws den energopoieitai to timeout sto recv tou command
                //std::cout << "------- >>> HeartBeat\n";
            }


            else {
                std::cout << "----> AGNWSTO COMMAND!! :" << &cmdbuffer[0] << std::endl;
                //den einai kapoio gnwsto command opote termatizw ti syndesi me to xristi
                cleanup(myID, socketfd);
                return;
            }

        } catch (std::exception& ex) {
            //ean simvei kapoio exception p.x. den mporese na ginei proothisi dedomenwn
            //sto remote socket appla katapinw to sfalma.
            std::cout << "----------> EXCEPTION ********  Synevh exception sto dostuff while loop: " << ex.what() << std::endl;
            displayErrno("----------> EXCEPTION dostuff loop");

            cleanup(myID, socketfd);

            return;
        }
        catch ( ... ) {
            std::cout << "Unknown error in dostuff loop" << std::endl;
            return;
        }
    } // while proccessCommand
}

//Detecting endianness programmatically in a C++ program
//http://stackoverflow.com/questions/1001307/detecting-endianness-programmatically-in-a-c-program
void displayEndianness()
{
    if ( htonl(47) == 47 ) {
        std::cout << "System endianness: Big endian. [NOT TESTED YET]" << std::endl;
    } else {
        std::cout << "System endianness: Little endian. [OK]" << std::endl;
    }
}

void displayThreadInfo()
{
    unsigned int n = std::thread::hardware_concurrency();
    std::cout << n << " concurrent threads are supported.\n";
}

int main(int argc, char *argv[]) {
    int newsockfd;

    std::set_terminate(onTerminate);

    //gia na mi prokaleitai crash otan paw na grapsw se socket pou exei kleisei
    //http://stackoverflow.com/questions/108183/how-to-prevent-sigpipes-or-handle-them-properly
    #ifdef WIN32
        SOCKET socketfd = INVALID_SOCKET;
    #else
        int socketfd;
    signal(SIGPIPE, SIG_IGN);
    #endif

    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    std::cout << "Andama Proxy Server v1.0 BETA. (c) 2014 Yiannis Bourkelis" << std::endl;
    displayEndianness();
    displayThreadInfo();


#ifdef WIN32
    // Initialize Winsock
    int iResult;
    WSADATA wsaData;
    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        std::cout << "WSAStartup failed: " << iResult << std::endl;
        return -1;
    }
#endif

    socketfd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef WIN32
        if (socketfd == INVALID_SOCKET) {
#else
        if (socketfd < 0){
#endif
        error("ERROR opening socket");
        return -1;
        }

    bzero((char *) &serv_addr, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT_NUMBER);

    if (bind(socketfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(socketfd, 5);
    std::cout << "Listenning for connections on port: " << PORT_NUMBER << std::endl;

    clilen = sizeof(cli_addr);

    while (true) {
        try {
            newsockfd = accept(socketfd, (struct sockaddr *) &cli_addr, &clilen);
            //int pHandle=new int;
            if (newsockfd < 0)
                error("ERROR on accept");

            std::cout << getTime() << " " << std::this_thread::get_id() <<
                         " New client accepted. Kalw dostuff while loop se neo thread." << std::endl;

            //thetw to recv timeout
#ifdef WIN32
            int iTimeout = 90000;
            setsockopt(newsockfd,
                               SOL_SOCKET,
                               SO_RCVTIMEO,
                               (const char *)&iTimeout,
                               sizeof(iTimeout) );
#else
            struct timeval tv;
            tv.tv_sec = 90;  /* 90 Secs Timeout */
            tv.tv_usec = 0;  // Not init'ing this can cause strange errors
            setsockopt(newsockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,sizeof(struct timeval));
#endif
            //SIMANTIKO: kanw disable to nagle algorithm. meiwnei to latency.
            int flag = 1;
            setsockopt(newsockfd,                    /* socket affected */
                                    IPPROTO_TCP,     /* set option at TCP level */
                                    TCP_NODELAY,     /* name of option */
                                    (char *) &flag,  /* the cast is historical cruft */
                                    sizeof(int));    /* length of option value */

            //diaxeirizomai ton neo client se neo thread.
            std::thread(dostuff, newsockfd, cli_addr.sin_addr.s_addr).detach();
        } catch ( std::exception& ex) {
            std::cout << "Accept loop exception: " << ex.what() << std::endl;
        }
        catch ( ... ) {
            std::cout << "Unknown error in main loop" << std::endl;
        }
    } /* end of while */
}
