#include <iostream>
#include <memory>
#include <string>
#include <pthread.h>
#include <openssl/md5.h>
#include <grpcpp/grpcpp.h>
#include "demo.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using demo::BuyRequest;
using demo::BuyReply;
using demo::Buy;

#define NUM_THREADS     500
#define SALT "guessWhat"

struct threadInfo{
    int  threadId;
    int uid;
    int item_id;
    std::string token;
    int num;

};

std::string MD5(const std::string& src )
{
    MD5_CTX ctx;

    std::string md5_string;
    unsigned char md[16] = { 0 };
    char tmp[33] = { 0 };

    MD5_Init( &ctx );
    MD5_Update( &ctx, src.c_str(), src.size() );
    MD5_Final( md, &ctx );

    for( int i = 0; i < 16; ++i )
    {
        memset( tmp, 0x00, sizeof( tmp ) );
        sprintf( tmp, "%02X", md[i] );
        md5_string += tmp;
    }
    return md5_string;
}


class BuyClient {
public:
    BuyClient(std::shared_ptr<Channel> channel)
            : stub_(Buy::NewStub(channel)) {}


    std::string CreateOrder(const std::int32_t  uid, const std::string& token,
                            const std::int32_t item_id, const std::int32_t num) {
        BuyRequest request;
        request.set_uid(uid);
        request.set_token(token);
        request.set_item_id(item_id);
        request.set_num(num);

        BuyReply reply;
        ClientContext context;

        Status status = stub_->CreateOrder(&context, request, &reply);

        if (status.ok()) {
            return reply.message();
        } else {
            std::cout << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return "RPC failed";
        }
    }

private:
    std::unique_ptr<Buy::Stub> stub_;
};

void sendRequest(threadInfo* info) {

    BuyClient buyer(grpc::CreateChannel(
            "localhost:50051", grpc::InsecureChannelCredentials()));
    std::string ret;
    for (int i=0; i < 20; i++) {
        ret = buyer.CreateOrder(info->uid, info->token, info->item_id, info->num);
        printf("%s\n", ret.c_str());
    }

}

int main(int argc, char** argv) {

    pthread_t tids[NUM_THREADS];
    struct threadInfo threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i)
    {
        std::cout <<"main() : creating thread, " << i << std::endl;
        threads[i].threadId = i;
        threads[i].uid = i;
        threads[i].item_id = 10;
        threads[i].token = MD5(SALT + std::to_string(i));
        threads[i].num = 1;
        int ret = pthread_create(&tids[i], NULL, reinterpret_cast<void *(*)(void *)>(sendRequest), &threads[i]);
        if (ret != 0)
            std::cout << "pthread_create error: error_code=" << ret << std::endl;
    }
    pthread_exit(NULL);
    return 0;
}
