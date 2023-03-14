// Copyright 2022 Dennis Hezel
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define AGRPC_STANDALONE_ASIO

#include "hellostreamingworld.grpc.pb.h"
#include "server_shutdown_asio.hpp"

// #include "helper.hpp"
// #include "server_shutdown_asio.hpp"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <agrpc/asio_grpc.hpp>
#include <asio/as_tuple.hpp>
#include <asio/bind_executor.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/thread_pool.hpp>

#include <iostream>
#include <thread>

using Channel = asio::experimental::channel<void(std::error_code, hellostreamingworld::Request)>;

// This function will read one requests from the client at a time. Note that gRPC only allows calling agrpc::read after
// a previous read has completed.
asio::awaitable<void> reader(grpc::ServerAsyncReaderWriter<hellostreamingworld::Response, hellostreamingworld::Request>& reader_writer,
	Channel& channel) {
	while (true) {
		hellostreamingworld::Request request;
		if (!co_await agrpc::read(reader_writer, request)) {
			// Client is done writing.
			break;
		}
		// Send request to writer. The `max_buffer_size` of the channel acts as backpressure.
		(void)co_await channel.async_send(std::error_code{}, std::move(request),
			asio::as_tuple(asio::use_awaitable));
	}
	// Signal the writer to complete.
	channel.close();
}

// The writer will pick up reads from the reader through the channel and switch to the thread_pool to compute their
// response.
asio::awaitable<bool> writer(grpc::ServerAsyncReaderWriter<hellostreamingworld::Response, hellostreamingworld::Request>& reader_writer,
	Channel& channel, asio::thread_pool& thread_pool) {
	bool ok{true};
	while (ok) {
		const auto [ec, request] = co_await channel.async_receive(asio::as_tuple(asio::use_awaitable));
		if (ec) {
			// Channel got closed by the reader.
			break;
		}
		// In this example we switch to the thread_pool to compute the response.
		co_await asio::post(asio::bind_executor(thread_pool, asio::use_awaitable));
		// Compute the response.
		hellostreamingworld::Response response;
		response.set_integer(request.integer() * 2);

		// reader_writer is thread-safe so we can just interact with it from the thread_pool.
		ok = co_await agrpc::write(reader_writer, response);
		// Now we are back on the main thread.
	}
	co_return ok;
}

asio::awaitable<void> handle_bidirectional_streaming_request(
	hellostreamingworld::Example::AsyncService& service,
	asio::thread_pool& thread_pool) {
	grpc::ServerContext server_context;
	grpc::ServerAsyncReaderWriter<hellostreamingworld::Response, hellostreamingworld::Request> reader_writer{&server_context};
	bool request_ok = co_await agrpc::request(&hellostreamingworld::Example::AsyncService::RequestBidirectionalStreaming,
		service, server_context, reader_writer);
	if (!request_ok) {
		// Server is shutting down.
		co_return;
	}
	asio::co_spawn(co_await asio::this_coro::executor, handle_bidirectional_streaming_request(service, thread_pool), asio::detached);
	// Maximum number of requests that are buffered by the channel to enable backpressure.
	static constexpr auto MAX_BUFFER_SIZE = 2;

	Channel channel{co_await asio::this_coro::executor, MAX_BUFFER_SIZE};
	using namespace asio::experimental::awaitable_operators;
	const auto ok = co_await (reader(reader_writer, channel) && writer(reader_writer, channel, thread_pool));
	if (!ok) {
		// Client has disconnected or server is shutting down.
		co_return;
	}
	co_await agrpc::finish(reader_writer, grpc::Status::OK);
}

int main(int argc, const char** argv) {
	const auto port = argc >= 2 ? argv[1] : "50051";
	const auto host = std::string("0.0.0.0:") + port;

	std::unique_ptr<grpc::Server> server;

	grpc::ServerBuilder builder;

	agrpc::GrpcContext grpc_context{builder.AddCompletionQueue()};
	builder.AddListeningPort(host, grpc::InsecureServerCredentials());

	hellostreamingworld::Example::AsyncService service;
	builder.RegisterService(&service);
	server = builder.BuildAndStart();

	asio::thread_pool thread_pool{1};
	asio::co_spawn(grpc_context, handle_bidirectional_streaming_request(service, thread_pool), asio::detached);
	grpc_context.run();
	std::cout << "Shutdown completed\n";
}