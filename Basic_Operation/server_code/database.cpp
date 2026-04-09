#include <pqxx/pqxx>
#include <string>
#include <mutex> 
#include <queue>
#include <vector>

class Database{
public: 

    Database(const std::string& conn_str, int pool_size = 5) {
        for (int i = 0; i < pool_size; ++i) {
            pool.push(std::make_unique<pqxx::connection>(conn_str));
        }
    }

    ~Database() {
        // connections close automatically via pqxx::connection destructor
    }
    void execute(const std::string& query){
        auto conn = acquire();
        try{
            pqxx::work txn(*conn);
            txn.exec(query);
            txn.commit();
        }catch(...){
            release(std::move(conn));
            throw;
        }
        release(std::move(conn));
    }

    pqxx::result fetch(const std::string& query){
        auto conn = acquire();
        try{
            pqxx::work txn(*conn);
            pqxx::result res = txn.exec(query);
            txn.commit();
            release(std::move(conn));
            return res;
        }catch(...){
            release(std::move(conn));
            throw;
        }
    }

    template<typename Func>
    void transaction(Func&& fn){
        auto conn = acquire();
        try{
            pqxx::work txn(*conn);
            fn(txn);
            txn.commit();
        }catch(...){
            release(std::move(conn));
            throw;
        }
        release(std::move(conn));
    }

private:
    // pqxx::connection represents a connection to cpp
    std::queue<std::unique_ptr<pqxx::connection>> pool;
    std::mutex mtx;
    std::unique_ptr<pqxx::connection> acquire(){
        std::lock_guard<std::mutex> lock(mtx);
        if (pool.empty()) {
            throw std::runtime_error("No available connections");
        }
        std::unique_ptr<pqxx::connection> conn = std::move(pool.front());
        pool.pop();
        return conn;
    }
    void release(std::unique_ptr<pqxx::connection> conn){
        std::lock_guard<std::mutex> lock(mtx);
        pool.push(std::move(conn));
    }
};


