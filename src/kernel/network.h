#ifndef NETWORK_H_INCLUDED
#define NETWORK_H_INCLUDED

#include <memory>
#include <thread>

#include <SFML/Network.hpp>

#include "blockchain.h"

namespace CryptoKernel
{
    /**
    * This class provides a peer-to-peer network between multiple blockchains
    */
    class Network
    {
        public:
            /**
            * Constructs a network object with the given log and blockchain. Attempts
            * to connect to saved seeds or those specified in peers.txt. Causes the
            * program to listen for incoming connections on port 49000.
            *
            * @param log a pointer to the CK log to use
            * @param blockchain a pointer to the blockchain to sync
            */
            Network(CryptoKernel::Log* log, CryptoKernel::Blockchain* blockchain);

            /**
            * Default destructor
            */
            ~Network();

            /**
            * Returns the number of currently connected peers
            *
            * @return an unsigned integer with the number of connections
            */
            unsigned int getConnections();

            /**
            * Broadcast a set of transactions to connected peers
            *
            * @param transactions the transactions to broadcast
            */
            void broadcastTransactions(const std::vector<CryptoKernel::Blockchain::transaction> transactions);

            /**
            * Broadcast a block to connected peers
            *
            * @param block the block to broadcast
            */
            void broadcastBlock(const CryptoKernel::Blockchain::block block);

            /**
            * Returns an estimate of synchronisation progress
            *
            * @return a double representing the progress of synchronisation out of 1.0
            */
            double syncProgress();

        private:
            class Peer;

            struct PeerInfo
            {
                std::unique_ptr<Peer> peer;
                Json::Value info;
            };
            std::map<std::string, PeerInfo*> connected;

            CryptoKernel::Log* log;
            CryptoKernel::Blockchain* blockchain;

            CryptoKernel::Storage* peers;

            bool running;

            void networkFunc();
            std::unique_ptr<std::thread> networkThread;

            void connectionFunc();
            std::unique_ptr<std::thread> connectionThread;

            sf::TcpListener listener;
    };
}

#endif // NETWORK_H_INCLUDED
