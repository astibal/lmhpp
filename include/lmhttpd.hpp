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

#include <microhttpd.h>
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
                                  size_t* upload_data_size) = 0;

    };


    struct ResponseParams {
        ResponseParams() = default;

        unsigned short response_code = MHD_HTTP_OK;
        std::string response_message;

        std::vector<std::pair<std::string, std::string>> headers;
    };
/**
 * The dynamic controller is a controller for creating user defined pages.
 */
    class DynamicController:public Controller {
    public:
        bool validPath(const char* path, const char* method) override = 0;

        /**
         * User defined http response.
         */
        virtual ResponseParams createResponse(struct MHD_Connection* connection,
                                    const char* url, const char* method, const char* upload_data,
                                    size_t* upload_data_size, std::stringstream& response) = 0;

        int handleRequest(struct MHD_Connection* connection,
                                  const char* url, const char* method, const char* upload_data,
                                  size_t* upload_data_size) override {

            std::stringstream response_ss;
            auto const response_params = createResponse(connection, url, method, upload_data, upload_data_size, response_ss);

            //Send response.
            auto response_str = response_ss.str();
            struct MHD_Response * response = MHD_create_response_from_buffer(
                    response_str.size(),
                    response_str.data(), MHD_RESPMEM_MUST_COPY);

            for(auto const& [hdr, hdr_val]: response_params.headers ) {
                MHD_add_response_header(response, hdr.c_str(), hdr_val.c_str());
            }

            int ret = MHD_queue_response(connection, response_params.response_code, response);
            MHD_destroy_response(response);

            return ret;
        }
    };

    class WebServer{
    private:
        uint16_t port_;
        struct MHD_Daemon* daemon_;
        struct options_t {
            bool bind_loopback = false;

            // optional handlers
            std::optional<std::function<bool()>> handler_should_terminate;
        };
        options_t options_;

        /** List of controllers this server has. */
        std::vector<Controller*> controllers;

        static int request_handler(void * cls, struct MHD_Connection * connection,
                                   const char * url, const char * method, const char * version,
                                   const char * upload_data, size_t * upload_data_size, void ** ptr) {

            auto const* server = static_cast<WebServer*>(cls);

            Controller* controller = nullptr;
            for(auto* c: server->controllers){
                if(c->validPath(url, method)){
                    controller = c;
                    break;
                }
            }

            if(!controller){
                struct MHD_Response* response = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
                return MHD_queue_response (connection, MHD_HTTP_NOT_FOUND, response);
            }

            return controller->handleRequest(connection, url, method, upload_data, upload_data_size);
        }
    public:
        explicit WebServer(uint16_t p) : port_(p) {};

        options_t& options() { return options_; }
        options_t const& options() const { return options_; }


        void addController(Controller* controller){
            controllers.emplace_back(controller);
        };

        int start(){
            sockaddr_in bind_addr{};

            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(port_);
            if(options().bind_loopback) bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            daemon_ = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                                      port_, nullptr, nullptr,
                                      reinterpret_cast<MHD_AccessHandlerCallback>(&request_handler),
                                      this,
                                      MHD_OPTION_SOCK_ADDR, &bind_addr,
                                      MHD_OPTION_END);

            if(!daemon_)
                return 1;

            while(true){
                ::usleep(100*1000);

                if(options().handler_should_terminate.has_value()) {
                    auto const& sh_callback = options().handler_should_terminate.value();
                    if(sh_callback())
                        break;
                }
            }

            MHD_stop_daemon(daemon_);
            return 0;
        }
    };
}
#endif //LMHTTPD_HPP
