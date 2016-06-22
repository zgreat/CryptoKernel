#ifndef STORAGE_H_INCLUDED
#define STORAGE_H_INCLUDED

#include <mutex>
#include <leveldb/db.h>

namespace CryptoKernel
{
    class Storage
    {
        public:
            Storage(std::string filename);
            ~Storage();
            bool store(std::string key, std::string value);
            std::string get(std::string key);
            bool erase(std::string key);
            bool getStatus();
            class Iterator
            {
                public:
                    Iterator(leveldb::DB* db, std::mutex* dbMutex);
                    ~Iterator();
                    void SeekToFirst();
                    bool Valid();
                    void Next();
                    std::string key();
                    std::string value();
                    bool getStatus();

                private:
                    leveldb::Iterator* it;
                    std::mutex* dbMutex;
            };
            Iterator* newIterator();

        private:
            leveldb::DB* db;
            std::mutex dbMutex;
            bool status;
    };
}

#endif // STORAGE_H_INCLUDED