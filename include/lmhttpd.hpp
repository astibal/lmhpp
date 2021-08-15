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

/**
 * The dynamic controller is a controller for creating user defined pages.
 */
    class DynamicController:public Controller {
    public:
        bool validPath(const char* path, const char* method) override = 0;

        /**
         * User defined http response.
         */
        virtual void createResponse(struct MHD_Connection* connection,
                                    const char* url, const char* method, const char* upload_data,
                                    size_t* upload_data_size, std::stringstream& response) = 0;

        int handleRequest(struct MHD_Connection* connection,
                                  const char* url, const char* method, const char* upload_data,
                                  size_t* upload_data_size) override {

            std::stringstream response_ss;
            createResponse(connection, url, method, upload_data, upload_data_size, response_ss);

            //Send response.
            auto response_str = response_ss.str();
            struct MHD_Response * response = MHD_create_response_from_buffer(
                    response_str.size(),
                    response_str.data(), MHD_RESPMEM_MUST_COPY);

            int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);

            return ret;
        }
    };

    class WebServer{
    private:
        int port;
        struct MHD_Daemon* daemon;

        /** List of controllers this server has. */
        std::vector<Controller*> controllers;

        static int request_handler(void * cls, struct MHD_Connection * connection,
                                   const char * url, const char * method, const char * version,
                                   const char * upload_data, size_t * upload_data_size, void ** ptr) {

            std::cout << "Request: " << url << ", Method: " << method << std::endl;

            auto* server = static_cast<WebServer*>(cls);

            Controller* controller = nullptr;
            for(auto* c: server->controllers){
                if(c->validPath(url, method)){
                    controller = c;
                    break;
                }
            }

            if(!controller){
                std::cout << "Path not found.\n";
                struct MHD_Response* response = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
                return MHD_queue_response (connection, MHD_HTTP_NOT_FOUND, response);
            }

            return controller->handleRequest(connection, url, method, upload_data, upload_data_size);
        }
    public:
        explicit WebServer(int p) : port(p), daemon(nullptr) {};

        // optional handlers
        std::optional<std::function<bool()>> handler_should_terminate;

        void addController(Controller* controller){
            controllers.emplace_back(controller);
        };

        int start(){
            daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                                      port, nullptr, nullptr,
                                      reinterpret_cast<MHD_AccessHandlerCallback>(&request_handler),
                                      this,
                                      MHD_OPTION_END);

            if(!daemon)
                return 1;

            while(true){
                ::usleep(100*1000);

                if(handler_should_terminate.has_value()) {
                    auto& sh_callback = handler_should_terminate.value();
                    if(sh_callback())
                        break;
                }
            }

            MHD_stop_daemon(daemon);
            return 0;
        }
    };
}
#endif //LMHTTPD_HPP
