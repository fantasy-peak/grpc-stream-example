#define AGRPC_STANDALONE_ASIO

#include "hellostreamingworld.grpc.pb.h"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <agrpc/asio_grpc.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <iostream>
#include <thread>

asio::awaitable<void> make_bidirectional_streaming_request(hellostreamingworld::Example::Stub& stub) {
	grpc::ClientContext client_context;
	client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

	std::unique_ptr<grpc::ClientAsyncReaderWriter<hellostreamingworld::Request, hellostreamingworld::Response>> reader_writer;
	bool request_ok = co_await agrpc::request(&hellostreamingworld::Example::Stub::PrepareAsyncBidirectionalStreaming, stub,
		client_context, reader_writer);
	if (!request_ok) {
		// Channel is either permanently broken or transiently broken but with the fail-fast option.
		co_return;
	}

	// Let's perform a request/response ping-pong.
	hellostreamingworld::Request request;
	request.set_integer(1);
	hellostreamingworld::Response response;

	// Reads and writes can be performed simultaneously.
	using namespace asio::experimental::awaitable_operators;
	auto [read_ok, write_ok] = co_await (agrpc::read(reader_writer, response) && agrpc::write(reader_writer, request));

	int count{};
	while (read_ok && write_ok && count < 1) {
		std::cout << "Bidirectional streaming: " << response.integer() << '\n';
		request.set_integer(response.integer());
		++count;
		std::tie(read_ok, write_ok) =
			co_await (agrpc::read(reader_writer, response) && agrpc::write(reader_writer, request));
	}

	// Do not forget to signal that we are done writing before finishing.
	co_await agrpc::writes_done(reader_writer);

	grpc::Status status;
	co_await agrpc::finish(reader_writer, status);
}

int main(int argc, const char** argv) {
	const auto port = argc >= 2 ? argv[1] : "50051";
	const auto host = std::string("localhost:") + port;

	const auto channel = grpc::CreateChannel(host, grpc::InsecureChannelCredentials());

	hellostreamingworld::Example::Stub stub{channel};
	agrpc::GrpcContext grpc_context;

	asio::co_spawn(
		grpc_context,
		[&]() -> asio::awaitable<void> {
			using namespace asio::experimental::awaitable_operators;
			co_await make_bidirectional_streaming_request(stub);
		},
		asio::detached);

	grpc_context.run();
}