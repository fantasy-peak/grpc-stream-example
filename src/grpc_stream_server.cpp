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

#include <forward_list>
#include <fstream>
#include <iostream>
#include <sstream>
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
		std::cout << "post 1:" << std::this_thread::get_id() << std::endl;
		// reader_writer is thread-safe so we can just interact with it from the thread_pool.
		ok = co_await agrpc::write(reader_writer, response);
		std::cout << "post 2:" << std::this_thread::get_id() << std::endl;
		// Now we are back on the main thread.
	}
	co_return ok;
}

asio::awaitable<void> handle_bidirectional_streaming_request(
	grpc::ServerContext&, asio::thread_pool& thread_pool,
	grpc::ServerAsyncReaderWriter<hellostreamingworld::Response, hellostreamingworld::Request>& reader_writer) {
	std::cout << "handle_bidirectional_streaming_request: " << std::this_thread::get_id() << std::endl;
	static constexpr auto MAX_BUFFER_SIZE = 2;
	Channel channel{co_await asio::this_coro::executor, MAX_BUFFER_SIZE};
	using namespace asio::experimental::awaitable_operators;
	const auto ok = co_await (reader(reader_writer, channel) && writer(reader_writer, channel, thread_pool));
	if (!ok) {
		std::cout << "Client has disconnected or server is shutting down" << std::endl;
		co_return;
	}
	co_await agrpc::finish(reader_writer, grpc::Status::OK);
	std::cout << "END" << std::endl;
	co_return;
}

void readFile(std::string filename, std::string& content) {
	std::ifstream file(filename, std::ios::in);
	while (file.is_open()) {
		std::stringstream ss;
		ss << file.rdbuf();
		file.close();
		content = ss.str();
	}
}

int main(int argc, const char** argv) {
	const auto port = argc >= 2 ? argv[1] : "50051";
	const auto host = std::string("0.0.0.0:") + port;

	grpc::ServerBuilder builder;
	std::forward_list<agrpc::GrpcContext> grpc_contexts;
	for (size_t i = 0; i < 2; ++i) {
		grpc_contexts.emplace_front(builder.AddCompletionQueue());
	}
	bool enable_ssl = false;
	std::shared_ptr<grpc::ServerCredentials> creds = grpc::InsecureServerCredentials();
	if (enable_ssl) {
		std::string ca_crt_content;
		std::string server_crt_content;
		std::string server_key_content;
		readFile("ca.crt", ca_crt_content);
		readFile("server.crt", server_crt_content);
		readFile("server.key", server_key_content);
		grpc::SslServerCredentialsOptions sslOpts;
		grpc::SslServerCredentialsOptions::PemKeyCertPair keycert = {server_key_content, server_crt_content};
		grpc::SslServerCredentialsOptions ssl_opts;
		ssl_opts.pem_root_certs = ca_crt_content;
		ssl_opts.pem_key_cert_pairs.push_back(keycert);
		creds = grpc::SslServerCredentials(ssl_opts);
	}
	builder.AddListeningPort(host, creds);

	hellostreamingworld::Example::AsyncService service;
	builder.RegisterService(&service);
	std::unique_ptr<grpc::Server> server = builder.BuildAndStart();

	asio::thread_pool thread_pool{1};
	std::vector<std::thread> threads;
	for (size_t i = 0; i < 2; ++i) {
		threads.emplace_back([&, i] {
			auto& grpc_context = *std::next(grpc_contexts.begin(), i);
			agrpc::repeatedly_request(&hellostreamingworld::Example::AsyncService::RequestBidirectionalStreaming, service,
				asio::bind_executor(grpc_context,
					[&](grpc::ServerContext& server_context,
						grpc::ServerAsyncReaderWriter<hellostreamingworld::Response, hellostreamingworld::Request>& reader_writer)
						-> asio::awaitable<void> {
						co_await handle_bidirectional_streaming_request(server_context, thread_pool, reader_writer);
					}));
			grpc_context.run();
		});
	}
	asio::signal_set signals{grpc_contexts.front(), SIGINT, SIGTERM};
	signals.async_wait([&](const std::error_code&, int) {
		server->Shutdown();
		thread_pool.stop();
		for (auto& grpc_context : grpc_contexts)
			grpc_context.stop();
	});
	for (auto& thread : threads) {
		thread.join();
	}
	std::cout << "Shutdown completed\n";
}