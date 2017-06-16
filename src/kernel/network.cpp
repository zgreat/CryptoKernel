#include "network.h"
#include "networkpeer.h"
#include "version.h"

CryptoKernel::Network::Network(CryptoKernel::Log* log, CryptoKernel::Blockchain* blockchain)
{
    this->log = log;
    this->blockchain = blockchain;
    bestHeight = 0;

    myAddress = sf::IpAddress::getPublicAddress();

    networkdb.reset(new CryptoKernel::Storage("./peers"));
    peers.reset(new Storage::Table("peers"));

    std::unique_ptr<Storage::Transaction> dbTx(networkdb->begin());

    std::ifstream infile("peers.txt");
    if(!infile.is_open())
    {
        log->printf(LOG_LEVEL_ERR, "Network(): Could not open peers file");
    }

    std::string line;
    while(std::getline(infile, line))
    {
        if(!peers->get(dbTx.get(), line).isObject()) {
            Json::Value newSeed;
            newSeed["lastseen"] = 0;
            newSeed["height"] = 1;
            newSeed["score"] = 0;
            peers->put(dbTx.get(), line, newSeed);
        }
    }

    infile.close();

    dbTx->commit();

    if(listener.listen(49000) != sf::Socket::Done)
    {
        log->printf(LOG_LEVEL_ERR, "Network(): Could not bind to port 49000");
    }

    running = true;

    listener.setBlocking(false);

    // Start connection thread
    connectionThread.reset(new std::thread(&CryptoKernel::Network::connectionFunc, this));

    // Start management thread
    networkThread.reset(new std::thread(&CryptoKernel::Network::networkFunc, this));

    // Start peer thread
    peerThread.reset(new std::thread(&CryptoKernel::Network::peerFunc, this));
}

CryptoKernel::Network::~Network()
{
    running = false;
    connectionThread->join();
    networkThread->join();
    peerThread->join();
    listener.close();
}

void CryptoKernel::Network::peerFunc() {
    while(running) {
        bool wait = false;

        std::map<std::string, Json::Value> peerInfos;

        {
            std::lock_guard<std::recursive_mutex> lock(connectedMutex);
            CryptoKernel::Storage::Table::Iterator* it = new CryptoKernel::Storage::Table::Iterator(peers.get(), networkdb.get());

            for(it->SeekToFirst(); it->Valid(); it->Next())
            {
                if(connected.size() >= 8) {
                    wait = true;
                    break;
                }

                Json::Value peer = it->value();

                if(connected.find(it->key()) != connected.end()) {
                    continue;
                }

                std::time_t result = std::time(nullptr);

                const auto banIt = banned.find(it->key());
                if(banIt != banned.end()) {
                    if(banIt->second > static_cast<uint64_t>(result)) {
                        continue;
                    }
                }

                if(peer["lastattempt"].asUInt64() + 5 * 60 > static_cast<unsigned long long int> (result) && peer["lastattempt"].asUInt64() != peer["lastseen"].asUInt64()) {
                    continue;
                }

                sf::IpAddress addr(it->key());

                if(addr == sf::IpAddress::getLocalAddress()
                || addr == myAddress
                || addr == sf::IpAddress::LocalHost) {
                    continue;
                }

                log->printf(LOG_LEVEL_INFO, "Network(): Attempting to connect to " + it->key());

                peer["lastattempt"] = static_cast<unsigned long long int> (result);

                // Attempt to connect to peer
                sf::TcpSocket* socket = new sf::TcpSocket();
                if(socket->connect(it->key(), 49000, sf::seconds(3)) != sf::Socket::Done)
                {
                    log->printf(LOG_LEVEL_WARN, "Network(): Failed to connect to " + it->key());
                    delete socket;
                    peerInfos[it->key()] = peer;
                    break;
                }

                PeerInfo* peerInfo = new PeerInfo;
                peerInfo->peer.reset(new Peer(socket, blockchain, this));

                // Get height
                Json::Value info;
                try
                {
                    info = peerInfo->peer->getInfo();
                }
                catch(Peer::NetworkError& e)
                {
                    log->printf(LOG_LEVEL_WARN, "Network(): Error getting info from " + it->key());
                    delete peerInfo;
                    peerInfos[it->key()] = peer;
                    break;
                }

                log->printf(LOG_LEVEL_INFO, "Network(): Successfully connected to " + it->key());

                // Update info
                try {
                    peer["height"] = info["tipHeight"].asUInt64();
                } catch(const Json::Exception& e) {
                    log->printf(LOG_LEVEL_WARN, "Network(): " + it->key() + " sent a malformed info message");
                    delete peerInfo;
                    peerInfos[it->key()] = peer;
                    break;
                }

                peer["lastseen"] = static_cast<unsigned long long int> (result);

                peer["score"] = 0;

                peerInfo->info = peer;

                connected[it->key()].reset(peerInfo);
                peerInfos[it->key()] = peer;
                break;
            }

            if(!it->Valid()) {
                wait = true;
            }

            delete it;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(connectedMutex);
            std::unique_ptr<Storage::Transaction> dbTx(networkdb->begin());

            for(const auto& peer : peerInfos) {
                peers->put(dbTx.get(), peer.first, peer.second);
            }

            for(std::map<std::string, std::unique_ptr<PeerInfo>>::iterator it = connected.begin(); it != connected.end(); it++)
            {
                try
                {
                    const Json::Value info = it->second->peer->getInfo();
                    try {
                        const std::string peerVersion = info["version"].asString();
                        if(peerVersion.substr(0, peerVersion.find(".")) != version.substr(0, version.find(".")))
                        {
                            log->printf(LOG_LEVEL_WARN, "Network(): " + it->first + " has a different major version than us");
                            throw Peer::NetworkError();
                        }

                        it->second->info["height"] = info["tipHeight"].asUInt64();

                        for(const Json::Value& peer : info["peers"]) {
                            sf::IpAddress addr(peer.asString());
                            if(addr != sf::IpAddress::None) {
                                if(!peers->get(dbTx.get(), addr.toString()).isObject()) {
                                    log->printf(LOG_LEVEL_INFO, "Network(): Discovered new peer: " + addr.toString());
                                    Json::Value newSeed;
                                    newSeed["lastseen"] = 0;
                                    newSeed["height"] = 1;
                                    newSeed["score"] = 0;
                                    peers->put(dbTx.get(), addr.toString(), newSeed);
                                }
                            } else {
                                changeScore(it->first, 10);
                                throw Peer::NetworkError();
                            }
                        }
                    } catch(const Json::Exception& e) {
                        changeScore(it->first, 50);
                        throw Peer::NetworkError();
                    }

                    const std::time_t result = std::time(nullptr);
                    it->second->info["lastseen"] = static_cast<unsigned long long int> (result);
                }
                catch(const Peer::NetworkError& e)
                {
                    log->printf(LOG_LEVEL_WARN, "Network(): Failed to contact " + it->first + ", disconnecting it");
                    it = connected.erase(it);
                    if(connected.size() < 1)
                    {
                        break;
                    }
                }
            }

            dbTx->commit();
        }

        if(wait) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

void CryptoKernel::Network::networkFunc()
{
    while(running)
    {
        //Determine best chain
        uint64_t currentHeight = blockchain->getBlockDB("tip").getHeight();
        uint64_t bestHeight = currentHeight;
        connectedMutex.lock();
        for(std::map<std::string, std::unique_ptr<PeerInfo>>::iterator it = connected.begin(); it != connected.end(); it++)
        {
            if(it->second->info["height"].asUInt64() > bestHeight)
            {
                bestHeight = it->second->info["height"].asUInt64();
            }
            const auto banIt = banned.find(it->first);
            if(banIt != banned.end()) {
                if(banIt->second > static_cast<uint64_t>(std::time(nullptr))) {
                    log->printf(LOG_LEVEL_WARN, "Network(): Disconnecting " + it->first + " for being banned");
                    it = connected.erase(it);
                    if(connected.size() < 1) {
                        break;
                    }
                }
            }
        }
        this->bestHeight = bestHeight;
        connectedMutex.unlock();

        log->printf(LOG_LEVEL_INFO, "Network(): Current height: " + std::to_string(currentHeight) + ", best height: " + std::to_string(bestHeight));

        //Detect if we are behind
        if(bestHeight > currentHeight)
        {
            std::lock_guard<std::recursive_mutex> lock(connectedMutex);
            for(std::map<std::string, std::unique_ptr<PeerInfo>>::iterator it = connected.begin(); it != connected.end(); it++)
            {
                if(it->second->info["height"].asUInt64() > currentHeight)
                {
                    std::vector<CryptoKernel::Blockchain::block> blocks;
                    //Try to submit the first block
                    //If it fails get the 200 block previous until we can submit a block

                    do
                    {
                        log->printf(LOG_LEVEL_INFO, "Network(): Downloading blocks " + std::to_string(currentHeight + 1) + " to " + std::to_string(currentHeight + 201));
                        try {
                            blocks = it->second->peer->getBlocks(currentHeight + 1, currentHeight + 201);
                        } catch(Peer::NetworkError& e) {
                            log->printf(LOG_LEVEL_WARN, "Network(): Failed to contact " + it->first + " " + e.what() + " while downloading blocks");
                            it = connected.erase(it);
                            break;
                        }

                        try {
                            blockchain->getBlockDB(blocks[0].getPreviousBlockId().toString());
                        } catch(const CryptoKernel::Blockchain::NotFoundException& e) {
                            if(currentHeight == 1) {
                                break;
                            } else {
                                currentHeight = std::max(1, (int)currentHeight - 200);
                                continue;
                            }
                        }

                        break;
                    } while(true);

                    for(unsigned int i = 0; i < blocks.size(); i++)
                    {

                        if(!blockchain->submitBlock(blocks[i]))
                        {
                            changeScore(it->first, 50);
                            break;
                        }
                    }

                    currentHeight = blockchain->getBlockDB("tip").getHeight();

                    if(connected.size() < 1) {
                        break;
                    }
                }
            }
        }

        //Rebroadcast unconfirmed transactions
        //broadcastTransactions(blockchain->getUnconfirmedTransactions());

        if(bestHeight == currentHeight) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20000));
        }
    }
}

void CryptoKernel::Network::connectionFunc()
{
    while(running)
    {
        sf::TcpSocket* client = new sf::TcpSocket();
        if(listener.accept(*client) == sf::Socket::Done)
        {
            std::lock_guard<std::recursive_mutex> lock(connectedMutex);
            if(connected.find(client->getRemoteAddress().toString()) != connected.end()) {
                log->printf(LOG_LEVEL_INFO, "Network(): Incoming connection duplicates existing connection for " + client->getRemoteAddress().toString());
                client->disconnect();
                delete client;
                continue;
            }

            const auto it = banned.find(client->getRemoteAddress().toString());
            if(it != banned.end()) {
                if(it->second > static_cast<uint64_t>(std::time(nullptr))) {
                    log->printf(LOG_LEVEL_INFO, "Network(): Incoming connection " + client->getRemoteAddress().toString() + " is banned");
                    client->disconnect();
                    delete client;
                    continue;
                }
            }

            sf::IpAddress addr(client->getRemoteAddress());

            if(addr == sf::IpAddress::getLocalAddress()
            || addr == myAddress
            || addr == sf::IpAddress::LocalHost) {
                log->printf(LOG_LEVEL_INFO, "Network(): Incoming connection " + client->getRemoteAddress().toString() + " is connecting to self");
                client->disconnect();
                delete client;
                continue;
            }


            log->printf(LOG_LEVEL_INFO, "Network(): Peer connected from " + client->getRemoteAddress().toString() + ":" + std::to_string(client->getRemotePort()));
            PeerInfo* peerInfo = new PeerInfo();
            peerInfo->peer.reset(new Peer(client, blockchain, this));

            Json::Value info;

            try
            {
                info = peerInfo->peer->getInfo();
            }
            catch(const Peer::NetworkError& e)
            {
                log->printf(LOG_LEVEL_WARN, "Network(): Failed to get information from connecting peer");
                delete peerInfo;
                continue;
            }

            try {
                peerInfo->info["height"] = info["tipHeight"].asUInt64();
            } catch(const Json::Exception& e) {
                log->printf(LOG_LEVEL_WARN, "Network(): Incoming peer sent invalid info message");
                delete peerInfo;
                continue;
            }

            const std::time_t result = std::time(nullptr);
            peerInfo->info["lastseen"] = static_cast<unsigned long long int> (result);

            peerInfo->info["score"] = 0;

            connected[client->getRemoteAddress().toString()].reset(peerInfo);

            std::unique_ptr<Storage::Transaction> dbTx(networkdb->begin());
            peers->put(dbTx.get(), client->getRemoteAddress().toString(), peerInfo->info);
            dbTx->commit();
        }
        else
        {
            delete client;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

unsigned int CryptoKernel::Network::getConnections()
{
    return connected.size();
}

void CryptoKernel::Network::broadcastTransactions(const std::vector<CryptoKernel::Blockchain::transaction> transactions)
{
    for(std::map<std::string, std::unique_ptr<PeerInfo>>::iterator it = connected.begin(); it != connected.end(); it++)
    {
        try {
            it->second->peer->sendTransactions(transactions);
        } catch(CryptoKernel::Network::Peer::NetworkError& err) {
            log->printf(LOG_LEVEL_WARN, "Network::broadcastTransactions(): Failed to contact peer");
        }
    }
}

void CryptoKernel::Network::broadcastBlock(const CryptoKernel::Blockchain::block block)
{
    for(std::map<std::string, std::unique_ptr<PeerInfo>>::iterator it = connected.begin(); it != connected.end(); it++)
    {
        try {
            it->second->peer->sendBlock(block);
        } catch(CryptoKernel::Network::Peer::NetworkError& err) {
            log->printf(LOG_LEVEL_WARN, "Network::broadcastBlock(): Failed to contact peer");
        }
    }
}

double CryptoKernel::Network::syncProgress()
{
    const uint64_t currentHeight = blockchain->getBlockDB("tip").getHeight();
    return (double)(currentHeight)/(double)(bestHeight);
}

void CryptoKernel::Network::changeScore(const std::string& url, const uint64_t score) {
    connected[url]->info["score"] = connected[url]->info["score"].asUInt64() + score;
    log->printf(LOG_LEVEL_WARN, "Network(): " + url + " misbehaving, increasing ban score by " + std::to_string(score) + " to " + connected[url]->info["score"].asString());
    if(connected[url]->info["score"].asUInt64() > 200) {
        log->printf(LOG_LEVEL_WARN, "Network(): Banning " + url + " for being above the ban score threshold");
        // Ban for 24 hours
        banned[url] = static_cast<uint64_t>(std::time(nullptr)) + 24 * 60 * 60;
    }
}

std::set<std::string> CryptoKernel::Network::getConnectedPeers() {
    std::set<std::string> peerUrls;
    for(const auto& peer : connected) {
        peerUrls.insert(peer.first);
    }

    return peerUrls;
}
