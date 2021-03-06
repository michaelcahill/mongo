/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/config.h"

#ifdef MONGO_CONFIG_SSL

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_asio.h"

#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"

namespace mongo {
namespace executor {

class NetworkInterfaceASIO::AsyncSecureStream final : public AsyncStreamInterface {
public:
    AsyncSecureStream(asio::io_service* io_service, asio::ssl::context* sslContext)
        : _stream(*io_service, *sslContext) {}

    void connect(const asio::ip::tcp::resolver::iterator endpoints,
                 ConnectHandler&& connectHandler) override {
        // Stash the connectHandler as we won't be able to call it until we re-enter the state
        // machine.
        _userHandler = std::move(connectHandler);
        asio::async_connect(_stream.lowest_layer(),
                            std::move(endpoints),
                            [this](std::error_code ec, asio::ip::tcp::resolver::iterator iter) {
                                if (ec) {
                                    return _userHandler(ec);
                                }
                                return _handleConnect(ec, std::move(iter));
                            });
    }

    void write(asio::const_buffer buffer, StreamHandler&& streamHandler) override {
        asio::async_write(_stream, asio::buffer(buffer), std::move(streamHandler));
    }

    void read(asio::mutable_buffer buffer, StreamHandler&& streamHandler) override {
        asio::async_read(_stream, asio::buffer(buffer), std::move(streamHandler));
    }

private:
    void _handleConnect(std::error_code ec, asio::ip::tcp::resolver::iterator iter) {
        _stream.async_handshake(decltype(_stream)::client,
                                [this, iter](std::error_code ec) {
                                    if (ec) {
                                        return _userHandler(ec);
                                    }
                                    return _handleHandshake(ec, iter->host_name());
                                });
    }

    void _handleHandshake(std::error_code ec, const std::string& hostName) {
        auto certStatus =
            getSSLManager()->parseAndValidatePeerCertificate(_stream.native_handle(), hostName);
        if (!certStatus.isOK()) {
            warning() << certStatus.getStatus();
            return _userHandler(
                // TODO: fix handling of std::error_code w.r.t. codes used by Status
                std::error_code(certStatus.getStatus().code(), std::generic_category()));
        }
        _userHandler(std::error_code());
    }

    asio::ssl::stream<asio::ip::tcp::socket> _stream;
    ConnectHandler _userHandler;
};

void NetworkInterfaceASIO::_setupSecureSocket(AsyncOp* op,
                                              asio::ip::tcp::resolver::iterator endpoints) {
    auto secureStream = stdx::make_unique<AsyncSecureStream>(&_io_service, _sslContext.get_ptr());

    // See TODO in _setupSocket for how this call may change.
    op->setConnection(AsyncConnection(std::move(secureStream), rpc::supports::kOpQueryOnly));

    auto& stream = op->connection().stream();
    stream.connect(std::move(endpoints),
                   [this, op](std::error_code ec) {
                       _validateAndRun(op, ec, [this, op] { _authenticate(op); });
                   });
}

}  // namespace executor
}  // namespace mongo

#endif
