#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpc/support/log.h>
#include <grpcpp/grpcpp.h>
#include "hellostreamingworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::Status;
using hellostreamingworld::HelloReply;
using hellostreamingworld::HelloRequest;
using hellostreamingworld::MultiGreeter;

#include "hellostreamingworld.grpc.pb.h"

using grpc::Server;
using grpc::ServerAsyncWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using hellostreamingworld::HelloReply;
using hellostreamingworld::HelloRequest;
using hellostreamingworld::MultiGreeter;

class GreeterClient {
public:
	GreeterClient(std::shared_ptr<Channel> channel)
		: stub_(MultiGreeter::NewStub(channel)) {}

	void SayHello(const std::string& user, const std::string& num_greetings) {
		HelloRequest request;
		request.set_name(user);
		request.set_num_greetings(num_greetings);

		ClientContext context;
		std::unique_ptr<ClientReader<HelloReply>> reader(stub_->sayHello(&context, request));

		HelloReply reply;
		while (reader->Read(&reply)) {
			std::cout << "Got reply: " << reply.message() << std::endl;
		}

		Status status = reader->Finish();
		if (status.ok()) {
			std::cout << "sayHello rpc succeeded." << std::endl;
		}
		else {
			std::cout << "sayHello rpc failed." << std::endl;
			std::cout << status.error_code() << ": " << status.error_message() << std::endl;
		}
	}

private:
	std::unique_ptr<MultiGreeter::Stub> stub_;
};

// https://github.com/G-Research/grpc_async_examples
class ServerImpl final {
public:
	~ServerImpl() {
		server_->Shutdown();
		cq_->Shutdown();
	}

	void Run() {
		std::string server_address("0.0.0.0:50051");

		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service_);

		cq_ = builder.AddCompletionQueue();
		server_ = builder.BuildAndStart();
		std::cout << "Server listening on " << server_address << std::endl;

		HandleRpcs();
	}

private:
	class CallData {
	public:
		CallData(MultiGreeter::AsyncService* service, ServerCompletionQueue* cq)
			: service_(service)
			, cq_(cq)
			, responder_(&ctx_)
			, status_(CREATE)
			, times_(0) {
			Proceed();
		}

		void Proceed() {
			if (status_ == CREATE) {
				status_ = PROCESS;
				service_->RequestsayHello(&ctx_, &request_, &responder_, cq_, cq_, this);
			}
			else if (status_ == PROCESS) {
				// Now that we go through this stage multiple times,
				// we don't want to create a new instance every time.
				// Refer to gRPC's original example if you don't understand
				// why we create a new instance of CallData here.
				if (times_ == 0) {
					new CallData(service_, cq_);
				}

				if (times_++ >= 3) {
					status_ = FINISH;
					responder_.Finish(Status::OK, this);
				}
				else {
					std::string prefix("Hello ");
					reply_.set_message(prefix + request_.name() + ", no " + request_.num_greetings());

					responder_.Write(reply_, this);
				}
			}
			else {
				GPR_ASSERT(status_ == FINISH);
				delete this;
			}
		}

	private:
		MultiGreeter::AsyncService* service_;
		ServerCompletionQueue* cq_;
		ServerContext ctx_;

		HelloRequest request_;
		HelloReply reply_;

		ServerAsyncWriter<HelloReply> responder_;

		int times_;

		enum CallStatus {
			CREATE,
			PROCESS,
			FINISH
		};
		CallStatus status_; // The current serving state.
	};

	void HandleRpcs() {
		new CallData(&service_, cq_.get());
		void* tag; // uniquely identifies a request.
		bool ok;
		while (true) {
			GPR_ASSERT(cq_->Next(&tag, &ok));
			GPR_ASSERT(ok);
			static_cast<CallData*>(tag)->Proceed();
		}
	}

	std::unique_ptr<ServerCompletionQueue> cq_;
	MultiGreeter::AsyncService service_;
	std::unique_ptr<Server> server_;
};

int main(int argc, char** argv) {
	std::thread([] {
		sleep(2);
		GreeterClient greeter(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));
		std::string user("world");
		greeter.SayHello(user, "123");
	}).detach();
	ServerImpl server;
	server.Run();
	return 0;
}