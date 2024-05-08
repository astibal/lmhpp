/*
 *
Copyright (c) 2021, Ales Stibal <astib@mag0.net>
All rights reserved.

Redistribution and use in source and binary forms, with or without
        modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
        IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
        FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
        DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
        SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
        CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#ifndef LMHTTPD_HPP
#define LMHTTPD_HPP

#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <microhttpd.h>

#include <memory>
#include <iostream>
#include <cstring>
#include <vector>
#include <sstream>
#include <optional>
#include <functional>

namespace lmh {

/**
 * Base controller for handling http requests.
 */

    class Controller;
    struct ConnectionState {
        explicit ConnectionState(Controller& controller) : conroller(controller) {}
        Controller& conroller;
        std::string request_data;

        bool response_sent = false;
        std::vector<std::pair<std::string,std::string>> response_headers;
        std::string response_data;

        uint32_t request_waiting_loop_counter = 0;
    };

    class Controller{

    public:
        virtual ~Controller() = default;
        /**
         * Check if given path and method are handled by this controller.
         */
        virtual bool validPath(const char* path, const char* method) = 0;

        /**
         * Handles given request.
         */
        virtual int handleRequest(struct MHD_Connection* connection,
                                  const char* url, const char* method, const char* upload_data,
                                  size_t* upload_data_size, void** ptr) = 0;
        virtual int handleComplete(struct MHD_Connection* connection, enum MHD_RequestTerminationCode toe, ConnectionState* cs) {
            if(cs) delete cs;
            return MHD_YES;
        }
        virtual ConnectionState* create_state() { return new ConnectionState(*this); };
    };


    struct ResponseParams {
        ResponseParams() = default;

        unsigned short response_code = MHD_YES;
        std::string response_message;

        std::vector<std::pair<std::string, std::string>> headers;
    };
/**
 * The dynamic controller is a controller for creating user defined pages.
 */
    class DynamicController: public Controller {
    public:
        static inline timespec waiting_usec_sleep = { 0, 10000000 }; // 10 ms
        static inline uint32_t waiting_loops = 300; // this is ~ 3s

        bool validPath(const char* path, const char* method) override = 0;

        /**
         * User defined http response.
         */
        virtual ResponseParams createResponse(struct MHD_Connection* connection,
                                    const char* url, const char* method, const char* upload_data,
                                    size_t* upload_data_size, void** ptr, std::stringstream& response) = 0;

        int handleRequest(struct MHD_Connection* connection,
                                  const char* url, const char* method, const char* upload_data,
                                  size_t* upload_data_size, void** ptr) override {

            // state is destroyed in specific handler
            if(not *ptr) *ptr = create_state();

            auto* state = reinterpret_cast<lmh::ConnectionState*>(*ptr);

            // default return is - continue with connection
            int ret = MHD_YES;
            if(not state->response_sent) {


                std::string meth(method);
                // response not sent, because we did not receive any POST data yet.
                // ptr is now set, we can return and wait for data to arrive.
                if(meth == "POST" and upload_data == nullptr and state->response_data.empty()) {

                    // request timeout - empty request
                    if(++state->request_waiting_loop_counter > DynamicController::waiting_loops)
                        return MHD_NO;

                    timespec remaining;
                    nanosleep(&DynamicController::waiting_usec_sleep, &remaining);

                    // we don't care if we won't succeed with sleep, we will get here multiple times
                    // so few cycles more doesn't matter.

                    return MHD_YES;
                }

                // it response is not created yet, call createResponse & co
                if(state->response_data.empty()) {
                    std::stringstream response_ss;
                    auto const response_params = createResponse(connection, url, method, upload_data, upload_data_size,
                                                                ptr,
                                                                response_ss);

                    // we should not continue with connection, bail out now
                    if(response_params.response_code == MHD_NO) {
                        return MHD_NO;
                    } else {
                        state->response_data = response_ss.str();
                        state->response_headers = response_params.headers;
                    }
                }

                auto *response = MHD_create_response_from_buffer(
                        state->response_data.size(),
                        (void *) state->response_data.c_str(), MHD_RESPMEM_MUST_COPY);

                            for(auto const& [hdr, hdr_val]: state->response_headers ) {
                                MHD_add_response_header(response, hdr.c_str(), hdr_val.c_str());
                            }

                ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
                if (ret == MHD_YES) {
                    state->response_sent = true;
                }

                MHD_destroy_response(response);
            }

            // except handlers won't say otherwise, we continue with connection
            return MHD_YES;
        }
    };

    class WebServer{
    private:
        uint16_t port_;
        MHD_Daemon* daemon_ = nullptr;
        struct options_t {
            bool bind_loopback = false;
            std::string bind_address;
            std::string bind_interface;

            std::optional<std::pair<std::string, std::string>> certificate;

            // optional handlers
            std::optional<std::function<bool()>> handler_should_terminate;
            std::vector<std::string> allowed_ips = { "*", };

            bool is_allowed_ip(std::string_view ip) const {
                return std::any_of(allowed_ips.begin(), allowed_ips.end(),
                                   [&](auto const& it){
                                       if(it == "all" or it == "*") {
                                           return true;
                                       }
                                       return it == ip;
                                   });
            };
        };
        options_t options_;

        /** List of controllers this server has. */
        std::vector<std::shared_ptr<Controller>> controllers;

        static int request_handler(void * cls, struct MHD_Connection * connection,
                                   const char * url, const char * method, const char * version,
                                   const char * upload_data, size_t * upload_data_size, void ** ptr) {


            auto const* server = static_cast<WebServer*>(cls);

            if (!server->is_ip_allowed(connection)) {
                return MHD_queue_response(connection, MHD_HTTP_FORBIDDEN,
                                          MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT));
            }


            for(auto const& c: server->controllers){
                if(c and c->validPath(url, method)){
                    return c->handleRequest(connection, url, method, upload_data, upload_data_size, ptr);
                }
            }

            struct MHD_Response* response = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
            return MHD_queue_response (connection, MHD_HTTP_NOT_FOUND, response);
        }

        static void request_complete_handler(void *cls, struct MHD_Connection* connection, void **con_cls, enum MHD_RequestTerminationCode toe) {

            auto* cs = static_cast<struct ConnectionState*>(*con_cls);

            if(cs)
                cs->conroller.handleComplete(connection, toe, cs);
        }

    public:
        explicit WebServer(uint16_t p) : port_(p) {};

        options_t& options() { return options_; }
        options_t const& options() const { return options_; }


        void addController(std::shared_ptr<Controller> const& controller){
            controllers.emplace_back(controller);
        };

        bool is_daemon_alive() {

            auto const* fd_info = MHD_get_daemon_info(daemon_, MHD_DAEMON_INFO_LISTEN_FD);
            if (not fd_info || ::fcntl(fd_info->listen_fd, F_GETFL) == -1) {
                return false;
            }
            return true;
        }

        void start_daemon() {

            stop_daemon();

            auto sleepy = [](auto l) {
                timespec ts{};
                ts.tv_sec = l;
                nanosleep(&ts, nullptr);
            };

            int attempts = 12;
            while(! daemon_ && attempts >= 0) {
                sockaddr_in bind_addr{};

                memset(&bind_addr, 0, sizeof(bind_addr));
                bind_addr.sin_family = AF_INET;
                bind_addr.sin_port = htons(port_);
                if(options().bind_loopback) {
                    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                }
                else {
                    if(! options().bind_address.empty())
                        inet_pton(AF_INET, options().bind_address.c_str(), &bind_addr.sin_addr);
                }

                auto listen_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (listen_socket == -1) {
                    sleepy(5);
                    continue;
                }

                if(! options().bind_interface.empty()) {
                    auto ret = setsockopt(listen_socket, SOL_SOCKET, SO_BINDTODEVICE, options().bind_interface.c_str(),
                               static_cast<unsigned int>(options().bind_interface.size()));
                    if(ret < 0) {
                        close(listen_socket);
                        sleepy(5);
                        continue;
                    }
                }

                if (bind(listen_socket, (sockaddr*)&bind_addr, sizeof(bind_addr)) == -1) {
                    close(listen_socket);
                    sleepy(5);
                    continue;
                }

                if (listen(listen_socket, SOMAXCONN) == -1) {
                    close(listen_socket);
                    sleepy(5);
                    continue;
                }

                if(options().certificate.has_value()) {

                    auto key_src = options().certificate->first;
                    auto cert_src = options().certificate->second;

                    daemon_ = MHD_start_daemon(MHD_USE_EPOLL_INTERNALLY | MHD_USE_SSL,
                                               port_, nullptr, nullptr,
                                               reinterpret_cast<MHD_AccessHandlerCallback>(&request_handler),
                                               this,
                                               MHD_OPTION_LISTEN_SOCKET, listen_socket,
                                               MHD_OPTION_NOTIFY_COMPLETED,
                                               reinterpret_cast<MHD_RequestCompletedCallback>(request_complete_handler),
                                               nullptr,
                                               MHD_OPTION_HTTPS_MEM_KEY, key_src.c_str(),
                                               MHD_OPTION_HTTPS_MEM_CERT, cert_src.c_str(),
                                               MHD_OPTION_END);

                } else {
                    daemon_ = MHD_start_daemon(MHD_USE_EPOLL_INTERNALLY,
                                               port_, nullptr, nullptr,
                                               reinterpret_cast<MHD_AccessHandlerCallback>(&request_handler),
                                               this,
                                               MHD_OPTION_LISTEN_SOCKET, listen_socket,
                                               MHD_OPTION_NOTIFY_COMPLETED,
                                               reinterpret_cast<MHD_RequestCompletedCallback>(request_complete_handler),
                                               nullptr,
                                               MHD_OPTION_END);
                }

                if(! daemon_) {
                    timespec wait{};
                    timespec remain{};
                    wait.tv_sec = 5;
                    nanosleep(&wait, &remain);
                }

                --attempts;
            }
        }

        void stop_daemon() {
            if(daemon_)
                MHD_stop_daemon(daemon_);
        }

        int start(){

            start_daemon();

            timespec sleeptime {};
            sleeptime.tv_sec = 1;


            while(true){
                nanosleep(&sleeptime, nullptr);

                if(not is_daemon_alive()) {
                    start_daemon();
                }

                if(options().handler_should_terminate.has_value()) {
                    auto const& sh_callback = options().handler_should_terminate.value();
                    if(sh_callback())
                        break;
                }
            }

            stop_daemon();
            return true;
        }

        static std::string connection_ip(MHD_Connection *connection) {
            // Get the IP address of the client

            std::string ip;

            auto const* ci = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
            if (nullptr == ci || nullptr == ci->client_addr) {
                return ip;
            }
            std::array<char, INET6_ADDRSTRLEN>client_ip {0};

            if (ci->client_addr->sa_family == AF_INET) { // IPv4
                auto *addr = (struct sockaddr_in const*) ci->client_addr;
                inet_ntop(AF_INET, &addr->sin_addr, client_ip.data(), client_ip.size());
            }
            else if (ci->client_addr->sa_family == AF_INET6) { // IPv6
                auto *addr = (struct sockaddr_in6 const*) ci->client_addr;
                inet_ntop(AF_INET6, &addr->sin6_addr, client_ip.data(), client_ip.size());
            }
            else {
                return ip;
            }

            ip.assign(client_ip.begin(), std::find(client_ip.begin(), client_ip.end(), '\0'));
            return ip;
        }

        bool is_ip_allowed(MHD_Connection *connection) const {

            auto ip = connection_ip(connection);
            return options().is_allowed_ip(ip);
        }
    };
}
#endif //LMHTTPD_HPP
