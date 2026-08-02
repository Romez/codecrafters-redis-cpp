#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>

#include "resp.hpp"

using asio::ip::tcp;

constexpr int port = 6379;

// struct ReadBuf {
//     char* data;
//     size_t cap = 0;
//     size_t begin = 0;
//     size_t end = 0;
// };

// void ensure_buf_cap(ReadBuf& buf, size_t need) {
//     std::println("cap: {}, begin: {}, end: {}", buf.cap, buf.begin, buf.end);

//     size_t free_bytes = buf.cap - buf.end;
//     if (free_bytes >= need) return;

//     // compact
//     // if (buf.begin > 0) {
//     //     size_t len = buf.end - buf.begin;
//     //     std::memmove(buf.data, buf.data + buf.begin, len);
//     // }

//     free_bytes = buf.cap - buf.end;
//     if (free_bytes < need) {
//         size_t next_cap = buf.cap + need;

//         char* next_buf = new char[next_cap];

//         std::memcpy(next_buf, buf.data, buf.end);
        
//         delete buf.data;

//         buf.data = next_buf;
//         buf.cap = next_cap;

//     }
// }

// constexpr size_t read_buf_size = 128;

// asio::awaitable<void> read_loop(tcp::socket socket) {
//     ReadBuf buf{
//         .data = new char[128],
//         .cap = 128
//     };

//     try {
//         while(true) {
//             // ensure_buf_cap(buf, read_buf_size);

//             size_t bytes_read = co_await socket.async_read_some(asio::buffer(buf.data + buf.end, 10), asio::use_awaitable);
//             buf.end += bytes_read;

//             std::println("Bytes read: {}, end: {}", bytes_read, buf.end);

//             std::println("S: {}", buf.data);

//             /*
//                 size_t curr = 0;

//             while (curr < bytes_read) {
//                 char c = read_buf[curr++];

//                 if (c == '+') { // string
//                     cmd_start = curr;

//                     while(curr < read_buf_size) {
//                         char c = read_buf[curr++];
//                         if (c == '\n') {

//                         }
//                     }
//                 }
//                 else if (c == '$') { // bulk string

//                 }
//                 else if (c == '*') { // array

//                 }
//                 else {

//                 }
//             }
//             */
//         }
//     }
//     catch (const asio::system_error& e) {
//         if (e.code() == asio::error::eof) {
//             std::println("Client disconnected");
//         }
//         else {
//             std::println("Read socket failure: {}", e.code().message());
//         }
//     }
//     catch (const std::exception& e) {
//         std::println("Read failure: {}", e.what());
//     }
// }

asio::awaitable<void> accept_loop(tcp::acceptor&& acceptor) {
    auto io = acceptor.get_executor();

    try {
        while(true) {
            auto socket = co_await acceptor.async_accept(io, asio::use_awaitable);

            auto msg = resp_simple_string("PONG");
            co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);

            // asio::co_spawn(io, read_loop(std::move(socket)), asio::detached);
        }
    }
    catch (const asio::system_error& e) {
        std::println("Accept error: {}", e.code().message());
    }
    catch (const std::exception& e) {
      std::println("Unexpected exception: {}", e.what());
    }

    co_return;
}

int main() {
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    asio::io_context io;
    auto acceptor = tcp::acceptor(io, tcp::endpoint(tcp::v4(), port));

    asio::co_spawn(io, accept_loop(std::move(acceptor)), asio::detached);

    std::println("Server on port: {}", port);

    io.run();
    
    // int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // if (server_fd < 0) {
    //  std::cerr << "Failed to create server socket\n";
    //  return 1;
    // }
    
    // // Since the tester restarts your program quite often, setting SO_REUSEADDR
    // // ensures that we don't run into 'Address already in use' errors
    // int reuse = 1;
    // if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    //   std::cerr << "setsockopt failed\n";
    //   return 1;
    // }
    
    // struct sockaddr_in server_addr;
    // server_addr.sin_family = AF_INET;
    // server_addr.sin_addr.s_addr = INADDR_ANY;
    // server_addr.sin_port = htons(port);
    
    // if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    //   std::cerr << "Failed to bind to port 6379\n";
    //   return 1;
    // }

    // int connection_backlog = 5;
    // if (listen(server_fd, connection_backlog) != 0) {
    //   std::cerr << "listen failed\n";
    //   return 1;
    // }
    
    // struct sockaddr_in client_addr;
    // int client_addr_len = sizeof(client_addr);
    // std::cout << "Waiting for a client to connect...\n";

    // // You can use print statements as follows for debugging, they'll be visible when running tests.
    // std::cout << "Logs from your program will appear here!\n";

    // // Uncomment the code below to pass the first stage
    // accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);
    // std::cout << "Client connected\n";
    
    // close(server_fd);

    return 0;
}
