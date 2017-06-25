#include "keepalive.h"
#include "osxobjectivecbridge.h"


/*
 * i klasi afti exei 2 stoxous:
 * 1. na stelnei hertbeat ston server wste o server na kserei oti o client einai syndedemenos, wste ean den lavei heartbeat na ton aposyndesei
 * 2. na ketalavei ean exei diakopoei i syndesi me ton server kai na prospathisei na tin apokatastisei
 *
 * Ston server tha stelnei heartbeat kathe 30 sec. O server tha exei timeout sto recv 90 sec
 */

const int HEARTBEAT_RATE = 30; //seconds (default 30)


void keepalive::run(void){
    std::chrono::milliseconds sleep_dura(1000);
    setLastHeartBeat(std::chrono::high_resolution_clock::now());

    while (true){
        std::chrono::high_resolution_clock::time_point curr_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(curr_time - getLastHeartBeat());

        if (time_span.count() >= HEARTBEAT_RATE)
        {
            if (protocol->getConnectionState() != connectionState::disconnected){
                try {
                    setLastHeartBeat(std::chrono::high_resolution_clock::now()); //thetw ton xrono prin kalesw to sendHeartBeat, wste se periptwsi exception na mi kaleitai synexeia to sendHeartBeat
                    if (protocol->isRunning()){
                        protocol->sendHeartBeat();
                    } else {
                        protocol->setConnectionState(connectionState::disconnected);
                        protocol->start();
                    }
                } catch (std::exception& ex) {
                //    //TODO: thelei kaliteri diaxeirisi tis aposyndesis tou socket.
                    protocol->setConnectionState(connectionState::disconnected);
                    //protocol->start();
                } catch ( ... ) {
                    protocol->setConnectionState(connectionState::disconnected);
                    //protocol->start();
                }
            }
        }
        std::this_thread::sleep_for(sleep_dura);
    }
}

void keepalive::setLastHeartBeat(std::chrono::high_resolution_clock::time_point timepoint)
{
    _lastHeartBeat = timepoint;
}

std::chrono::high_resolution_clock::time_point keepalive::getLastHeartBeat()
{
    return _lastHeartBeat;
}
