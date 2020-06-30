
#include <memory>
#include <iostream>
#include <string>
#include <stdlib.h>
#include <stdexcept>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <hiredis/hiredis.h>
#include "mysql_driver.h"

#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/statement.h>
#include <openssl/md5.h>
#include "demo.grpc.pb.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using demo::BuyRequest;
using demo::BuyReply;
using demo::Buy;

#define DB_HOST "localhost"
#define DB_USER "user1"
#define DB_PASS "123456"
#define DB_NAME "shopping"
#define SALT "guessWhat"
#define REDIS_HOST "127.0.0.1"
#define REDIS_PORT 6379

const int SUCCESS_CODE = 0;
const int SOLD_OUT_CODE = 1;
const int INVALID_TOKEN_CODE = 2;
const int INVALID_NUM_CODE = 3;
const int BOUGHT_CODE = 4;
const int ERR_CODE = 5;


std::string MD5(const std::string& src)
{
    MD5_CTX ctx;

    std::string output;
    unsigned char md[16] = { 0 };
    char tmp[33] = { 0 };

    MD5_Init( &ctx );
    MD5_Update( &ctx, src.c_str(), src.size() );
    MD5_Final( md, &ctx );

    for ( int i = 0; i < 16; ++i )
    {
        memset( tmp, 0x00, sizeof( tmp ) );
        sprintf( tmp, "%02X", md[i] );
        output += tmp;
    }
    return output;
}

// 创建redis连接
redisContext* redisClient() {
    redisContext *conn = redisConnect(REDIS_HOST, REDIS_PORT);
    if (conn == NULL || conn->err) {
        if (conn) {
            printf("redis Error: %s\n", conn->errstr);
        } else {
            printf("Can't allocate redis context\n");
        }
        exit(0);
    }
    return conn;
}

void * safeRedisCommand(redisContext *c, const char *format, ...) {
    void* reply;
    reply = redisCommand(c, format);
    if (reply == NULL) {
        printf("redis execute error: %s\n", format);
        // 邮件等报警通知
    }
    return reply;
}




class ServerImpl final {
public:
    ~ServerImpl() {
        server_->Shutdown();
        cq_->Shutdown();
    }

    void Run(redisContext *redisConn, sql::Connection* dbConn) {
        std::string server_address("0.0.0.0:50051");

        ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

        builder.RegisterService(&service_);

        cq_ = builder.AddCompletionQueue();
        server_ = builder.BuildAndStart();
        std::cout << "Server listening on " << server_address << std::endl;

        HandleRpcs(redisConn, dbConn);
    }

private:
    class CallData {
    public:
        CallData(Buy::AsyncService* service, ServerCompletionQueue* cq, redisContext *redisConn, sql::Connection* dbConn)
                : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE) {
            Proceed(redisConn, dbConn);
        }

        void Proceed(redisContext *redisConn, sql::Connection* dbConn) {
            if (status_ == CREATE) {
                status_ = PROCESS;
                
                service_->RequestCreateOrder(&ctx_, &request_, &responder_, cq_, cq_,
                                          this);
            } else if (status_ == PROCESS) {
                new CallData(service_, cq_, redisConn, dbConn);

                int code = processOrder(&request_, redisConn, dbConn);
                reply_.set_code(code);
                reply_.set_message(getMessage(code));

                status_ = FINISH;
                responder_.Finish(reply_, Status::OK, this);
            } else {
                GPR_ASSERT(status_ == FINISH);
                delete this;
            }
        }

    private:

        Buy::AsyncService* service_;

        ServerCompletionQueue* cq_;

        ServerContext ctx_;

        BuyRequest request_;
        BuyReply reply_;

        ServerAsyncResponseWriter<BuyReply> responder_;

        enum CallStatus { CREATE, PROCESS, FINISH };
        CallStatus status_;
    };
    static bool isFirstOrder(const int uid, int itemId, redisContext *redisConn) {
        redisReply *rReply;
        rReply = static_cast<redisReply *>(safeRedisCommand(redisConn, "SISMEMBER users:%d %d", itemId, uid));
        return rReply->integer == 0;
    }
    // code和文本的映射
    static std::string getMessage(int code) {
        if (code < 0 || code >= 6) {
            return "invalid code";
        }
        std::string messages[6] = { "success",
                                  "sold out.",
                                  "token not valid",
                                  "only can buy 1 product",
                                  "you have bought 1 product",
                                  "sold out.",
                                  };
        return messages[code];
    }
    // 校验库存是否已经卖光
    static bool isSoldOut(const int item_id, redisContext *redisConn) {
        redisReply *reply;
        reply = static_cast<redisReply *>(safeRedisCommand(redisConn, "DECRBY stock:%d %d", item_id, 0));
        return reply->integer <= 0;
    }

    // 校验token
    static bool isValidUser(const int uid, const std::string token) {
        std::string tmp = std::to_string(uid);
        tmp = SALT + tmp;
        return MD5(tmp) == token;

    }

    // 实际生成订单和扣库存
    static int doCreateOrder(sql::Connection *con, int uid, int item_id, int num) {
        boost::scoped_ptr< sql::Statement > stmt(con->createStatement());
        int code;

        std::string stock_sql = "update item set stock=stock-1 where id=";
        stock_sql = stock_sql + std::to_string(item_id);
        std::string order_sql = "INSERT INTO orders(id, uid, item_id, num) VALUE ";
        order_sql = order_sql + "(null, " + std::to_string(uid) + "," + std::to_string(item_id) \
            + "," + std::to_string(num) + ")";

        try {
            stmt->execute("start transaction");
            stmt->execute(stock_sql);
            stmt->execute(order_sql);
            stmt->execute("commit");
            code = SUCCESS_CODE;

        }  catch (sql::SQLException &e) {
            // 报警通知
            printf("# ERR: ", e.what());
            code = ERR_CODE;
        }
        return code;

    }
    // 请求校验，包括订单数量、token、库存以及是否首次购买等判断
    static int checkRequest(BuyRequest* request, redisContext *redisConn) {
        if (request->num() != 1) {
            return INVALID_NUM_CODE;
        }
        if (!isValidUser(request->uid(), request->token())) {
            return INVALID_TOKEN_CODE;
        }
        if (isSoldOut(request->item_id(), redisConn)) {
            return SOLD_OUT_CODE;
        }
        if (!isFirstOrder(request->uid(), request->item_id(), redisConn)) {
            return BOUGHT_CODE;
        }
        return SUCCESS_CODE;
    }
    // 核心处理逻辑，包括token校验、库存检测及实际的订单生成和扣库存
    static int processOrder(BuyRequest* request, redisContext *redisConn, sql::Connection* dbConn){
        redisReply *rReply;
        int code;
        int uid, itemId, orderNum;
        uid = request->uid();
        itemId = request->item_id();
        orderNum = request->num();
        printf("%s\n", "**************************");
        printf("uid: %d want to buy item: %d, num: %d.\n", uid, itemId, orderNum);
        code = checkRequest(request, redisConn);
        if (code == SUCCESS_CODE) {
            // 在redis中尝试预扣库存，如果返回值小于0说明库存不足
            rReply = static_cast<redisReply *>(safeRedisCommand(redisConn, "DECRBY stock:%d %d", itemId, orderNum));
            if (rReply->integer < 0) {
                // 将预扣的库存补回去
                safeRedisCommand(redisConn, "INCRBY stock:%s %d", itemId, orderNum);
                code = SOLD_OUT_CODE;
            } else {
                code = doCreateOrder(dbConn, uid, itemId, orderNum);
                if (code == SUCCESS_CODE) {
                    safeRedisCommand(redisConn, "SADD users:%d %d", itemId, uid);
                }
            }
        }
        printf("%s\n", getMessage(code).c_str());
        return code;
    }

    void HandleRpcs(redisContext *redisConn, sql::Connection* dbConn) {
        new CallData(&service_, cq_.get(), redisConn, dbConn);
        void* tag;
        bool ok;
        while (true) {
            
            GPR_ASSERT(cq_->Next(&tag, &ok));
            GPR_ASSERT(ok);
            static_cast<CallData*>(tag)->Proceed(redisConn, dbConn);
        }
    }

    std::unique_ptr<ServerCompletionQueue> cq_;
    Buy::AsyncService service_;
    std::unique_ptr<Server> server_;
};

int main(int argc, char** argv) {

    ServerImpl server;
    redisContext *redisConn = redisClient();
    sql::Driver * driver = sql::mysql::get_driver_instance();
    sql::Connection *dbConn(driver->connect(DB_HOST, DB_USER, DB_PASS));
    dbConn->setSchema(DB_NAME);
    server.Run(redisConn, dbConn);
    return 0;
}
