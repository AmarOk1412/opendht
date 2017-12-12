/*
 *  Copyright (C) 2016 Savoir-faire Linux Inc.
 *  Author: Sébastien Blin <sebastien.blin@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#if OPENDHT_PROXY_CLIENT

#include "dht_proxy_client.h"

#include <chrono>
#include <json/json.h>
#include <restbed>
#include <vector>

#include "dhtrunner.h"

constexpr const char* const HTTP_PROTO {"http://"};

namespace dht {

DhtProxyClient::DhtProxyClient(const std::string& serverHost)
: serverHost_(serverHost), lockCurrentProxyInfos_(new std::mutex()),
  scheduler(DHT_LOG), currentProxyInfos_(new Json::Value())
{
    if (!serverHost_.empty())
        startProxy(serverHost_);
}

void
DhtProxyClient::confirmProxy()
{
    if (serverHost_.empty()) return;
    // Retrieve the connectivity each hours if connected, else every 5 seconds.
    auto disconnected_old_status =  statusIpv4_ == NodeStatus::Disconnected && statusIpv6_ == NodeStatus::Disconnected;
    getConnectivityStatus();
    auto disconnected_new_status = statusIpv4_ == NodeStatus::Disconnected && statusIpv6_ == NodeStatus::Disconnected;
    auto time = disconnected_new_status ? std::chrono::seconds(5) : std::chrono::hours(1);
    if (disconnected_old_status && !disconnected_new_status) {
        restartListeners();
    }
    auto confirm_proxy_time = scheduler.time() + time;
    scheduler.edit(nextProxyConfirmation, confirm_proxy_time);
}

void
DhtProxyClient::startProxy(const std::string& serverHost, const std::string& deviceKey)
{
    serverHost_ = serverHost;
    if (serverHost_.empty()) return;
    deviceKey_ = deviceKey;
    auto confirm_proxy_time = scheduler.time() + std::chrono::seconds(5);
    nextProxyConfirmation = scheduler.add(confirm_proxy_time, std::bind(&DhtProxyClient::confirmProxy, this));
    auto confirm_connectivity = scheduler.time() + std::chrono::seconds(5);
    nextConnectivityConfirmation = scheduler.add(confirm_connectivity, std::bind(&DhtProxyClient::confirmConnectivity, this));

    getConnectivityStatus();
}

void
DhtProxyClient::confirmConnectivity()
{
    // The scheduler must get if the proxy is disconnected
    auto confirm_connectivity = scheduler.time() + std::chrono::seconds(3);
    scheduler.edit(nextConnectivityConfirmation, confirm_connectivity);
}

DhtProxyClient::~DhtProxyClient()
{
    cancelAllOperations();
    cancelAllListeners();
}

void
DhtProxyClient::cancelAllOperations()
{
    std::lock_guard<std::mutex> lock(lockOperations_);
    auto operation = operations_.begin();
    while (operation != operations_.end()) {
        if (operation->thread.joinable()) {
            // Close connection to stop operation?
            restbed::Http::close(operation->req);
            operation->thread.join();
            operation = operations_.erase(operation);
        } else {
            ++operation;
        }
    }
}

void
DhtProxyClient::cancelAllListeners()
{
    std::lock_guard<std::mutex> lock(lockListener_);
    for (auto& listener: listeners_) {
        if (listener.thread.joinable()) {
            // Close connection to stop listener?
            if (listener.req)
                restbed::Http::close(listener.req);
            listener.thread.join();
        }
    }
}

void
DhtProxyClient::shutdown(ShutdownCallback cb)
{
    cancelAllOperations();
    cancelAllListeners();
    if (cb)
        cb();
}

NodeStatus
DhtProxyClient::getStatus(sa_family_t af) const
{
    switch (af)
    {
    case AF_INET:
        return statusIpv4_;
    case AF_INET6:
        return statusIpv6_;
    default:
        return NodeStatus::Disconnected;
    }
}

bool
DhtProxyClient::isRunning(sa_family_t af) const
{
    switch (af)
    {
    case AF_INET:
        return statusIpv4_ == NodeStatus::Connected;
    case AF_INET6:
        return statusIpv6_ == NodeStatus::Connected;
    default:
        return false;
    }
}

time_point
DhtProxyClient::periodic(const uint8_t*, size_t, const SockAddr&)
{
    // Exec all currently stored callbacks
    scheduler.syncTime();
    if (!callbacks_.empty()) {
        std::lock_guard<std::mutex> lock(lockCallbacks);
        for (auto& callback : callbacks_)
            callback();
        callbacks_.clear();
    }
    // Remove finished operations
    {
        std::lock_guard<std::mutex> lock(lockOperations_);
        auto operation = operations_.begin();
        while (operation != operations_.end()) {
            if (*(operation->finished)) {
                if (operation->thread.joinable()) {
                    // Close connection to stop operation?
                    restbed::Http::close(operation->req);
                    operation->thread.join();
                }
                operation = operations_.erase(operation);
            } else {
                ++operation;
            }
        }
    }
    return scheduler.run();
}

void
DhtProxyClient::get(const InfoHash& key, const GetCallback& cb, DoneCallback donecb, const Value::Filter& filterChain)
{
    restbed::Uri uri(HTTP_PROTO + serverHost_ + "/" + key.toString());
    auto req = std::make_shared<restbed::Request>(uri);

    auto finished = std::make_shared<std::atomic_bool>(false);
    Operation o;
    o.req = req;
    o.finished = finished;
    o.thread = std::thread([=](){
        // Try to contact the proxy and set the status to connected when done.
        // will change the connectivity status
        auto ok = std::make_shared<std::atomic_bool>(true);
        restbed::Http::async(req,
            [=](const std::shared_ptr<restbed::Request>& req,
                const std::shared_ptr<restbed::Response>& reply) {
            auto code = reply->get_status_code();

            if (code == 200) {
                try {
                    while (restbed::Http::is_open(req) and not *finished) {
                        restbed::Http::fetch("\n", reply);
                        if (*finished)
                            break;
                        std::string body;
                        reply->get_body(body);
                        reply->set_body(""); // Reset the body for the next fetch

                        std::string err;
                        Json::Value json;
                        Json::CharReaderBuilder rbuilder;
                        auto* char_data = reinterpret_cast<const char*>(&body[0]);
                        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
                        if (reader->parse(char_data, char_data + body.size(), &json, &err)) {
                            auto value = std::make_shared<Value>(json);
                            if ((not filterChain or filterChain(*value)) && cb) {
                                std::lock_guard<std::mutex> lock(lockCallbacks);
                                callbacks_.emplace_back([cb, value, finished]() {
                                    if (not cb({value}))
                                        *finished = true;
                                });
                            }
                        } else {
                            *ok = false;
                        }
                    }
                } catch (std::runtime_error& e) { }
            } else {
                *ok = false;
            }
        }).wait();
        if (donecb) {
            std::lock_guard<std::mutex> lock(lockCallbacks);
            callbacks_.emplace_back([=](){
                donecb(*ok, {});
            });
        }
        if (!ok) {
            // Connection failed, update connectivity
            getConnectivityStatus();
        }
        *finished = true;
    });
    {
        std::lock_guard<std::mutex> lock(lockOperations_);
        operations_.emplace_back(std::move(o));
    }
}

void
DhtProxyClient::get(const InfoHash& key, GetCallback cb, DoneCallback donecb,
                    Value::Filter&& filter, Where&& where)
{
    Query query {{}, where};
    auto filterChain = filter.chain(query.where.getFilter());
    get(key, cb, donecb, filterChain);
}

void
DhtProxyClient::put(const InfoHash& key, Sp<Value> val, DoneCallback cb, time_point, bool permanent)
{
    restbed::Uri uri(HTTP_PROTO + serverHost_ + "/" + key.toString());
    auto req = std::make_shared<restbed::Request>(uri);
    req->set_method("POST");
    auto json = val->toJson();
    if (permanent)
        json["permanent"] = true;
    Json::StreamWriterBuilder wbuilder;
    wbuilder["commentStyle"] = "None";
    wbuilder["indentation"] = "";
    auto body = Json::writeString(wbuilder, json) + "\n";
    req->set_body(body);
    req->set_header("Content-Length", std::to_string(body.size()));

    auto finished = std::make_shared<std::atomic_bool>(false);
    Operation o;
    o.req = req;
    o.finished = finished;
    o.thread = std::thread([=](){
        auto ok = std::make_shared<std::atomic_bool>(true);
        restbed::Http::async(req,
            [this, ok](const std::shared_ptr<restbed::Request>& /*req*/,
                        const std::shared_ptr<restbed::Response>& reply) {
            auto code = reply->get_status_code();

            if (code == 200) {
                restbed::Http::fetch("\n", reply);
                std::string body;
                reply->get_body(body);
                reply->set_body(""); // Reset the body for the next fetch

                try {
                    std::string err;
                    Json::Value json;
                    Json::CharReaderBuilder rbuilder;
                    auto* char_data = reinterpret_cast<const char*>(&body[0]);
                    auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
                    if (not reader->parse(char_data, char_data + body.size(), &json, &err))
                        *ok = false;
                } catch (...) {
                    *ok = false;
                }
            } else {
                *ok = false;
            }
        }).wait();
        if (cb) {
            std::lock_guard<std::mutex> lock(lockCallbacks);
            callbacks_.emplace_back([=](){
                cb(*ok, {});
            });
        }
        if (!ok) {
            // Connection failed, update connectivity
            getConnectivityStatus();
        }
        *finished = true;
    });
    {
        std::lock_guard<std::mutex> lock(lockOperations_);
        operations_.emplace_back(std::move(o));
    }
}

NodeStats
DhtProxyClient::getNodesStats(sa_family_t af) const
{
    lockCurrentProxyInfos_->lock();
    auto proxyInfos = *currentProxyInfos_;
    lockCurrentProxyInfos_->unlock();
    NodeStats stats {};
    auto identifier = af == AF_INET6 ? "ipv6" : "ipv4";
    try {
        stats = NodeStats(proxyInfos[identifier]);
    } catch (...) { }
    return stats;
}

Json::Value
DhtProxyClient::getProxyInfos() const
{
    restbed::Uri uri(HTTP_PROTO + serverHost_ + "/");
    auto req = std::make_shared<restbed::Request>(uri);

    // Try to contact the proxy and set the status to connected when done.
    // will change the connectivity status
    restbed::Http::async(req,
        [this](const std::shared_ptr<restbed::Request>&,
                       const std::shared_ptr<restbed::Response>& reply) {
        auto code = reply->get_status_code();

        if (code == 200) {
            restbed::Http::fetch("\n", reply);
            std::string body;
            reply->get_body(body);

            std::string err;
            Json::CharReaderBuilder rbuilder;
            auto* char_data = reinterpret_cast<const char*>(&body[0]);
            auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
            lockCurrentProxyInfos_->lock();
            try {
                reader->parse(char_data, char_data + body.size(), &*currentProxyInfos_, &err);
            } catch (...) {
                *currentProxyInfos_ = Json::Value();
            }
            lockCurrentProxyInfos_->unlock();
        } else {
            lockCurrentProxyInfos_->lock();
            *currentProxyInfos_ = Json::Value();
            lockCurrentProxyInfos_->unlock();
        }
    }).wait();
    lockCurrentProxyInfos_->lock();
    auto result = *currentProxyInfos_;
    lockCurrentProxyInfos_->unlock();
    return result;
}

std::vector<SockAddr>
DhtProxyClient::getPublicAddress(sa_family_t family)
{
    lockCurrentProxyInfos_->lock();
    auto proxyInfos = *currentProxyInfos_;
    lockCurrentProxyInfos_->unlock();
    // json["public_ip"] contains [ipv6:ipv4]:port or ipv4:port
    auto public_ip = proxyInfos["public_ip"].asString();
    if (!proxyInfos.isMember("public_ip") || (public_ip.length() < 2)) return {};
    std::string ipv4Address = "", ipv6Address = "", port = "";
    if (public_ip[0] == '[') {
        // ipv6 complient
        auto endIp = public_ip.find(']');
        if (public_ip.length() > endIp + 2) {
            port = public_ip.substr(endIp + 2);
            auto ips = public_ip.substr(1, endIp - 1);
            if (ips.find(".") != std::string::npos) {
                // Format: [ipv6:ipv4]:service
                auto ipv4And6Separator = ips.find_last_of(':');
                ipv4Address = ips.substr(ipv4And6Separator + 1);
                ipv6Address = ips.substr(0, ipv4And6Separator - 1);
            } else {
                // Format: [ipv6]:service
                ipv6Address = ips;
            }
        }
    } else {
        // Format: ipv4:service
        auto endIp = public_ip.find_last_of(':');
        port = public_ip.substr(endIp + 1);
        ipv4Address = public_ip.substr(0, endIp - 1);
    }
    switch (family)
    {
    case AF_INET:
        return SockAddr::resolve(ipv4Address, port);
    case AF_INET6:
        return SockAddr::resolve(ipv6Address, port);
    default:
        return {};
    }
}

size_t
DhtProxyClient::listen(const InfoHash& key, GetCallback cb, Value::Filter&& filter, Where&& where)
{
    restbed::Uri uri(HTTP_PROTO + serverHost_ + "/" + key.toString());
    auto req = std::make_shared<restbed::Request>(uri);
    req->set_method(deviceKey_.empty() ? "LISTEN" : "SUBSCRIBE");

    Query query {{}, where};
    auto filterChain = filter.chain(query.where.getFilter());
    auto pushNotifToken = std::make_shared<unsigned>(0);

    Listener l;
    ++listener_token_;
    l.key = key.toString();
    l.token = listener_token_;
    l.req = req;
    l.cb = cb;
    l.pushNotifToken = pushNotifToken;
    l.filterChain = std::move(filterChain);
    l.thread = std::thread([=]()
        {
            auto settings = std::make_shared<restbed::Settings>();
            if (deviceKey_.empty()) {
                std::chrono::milliseconds timeout(std::numeric_limits<int>::max());
                settings->set_connection_timeout(timeout); // Avoid the client to close the socket after 5 seconds.
            } else
                fillBodyToGetToken(req);

            struct State {
                std::atomic_bool ok {true};
                std::atomic_bool cancel {false};
            };
            auto state = std::make_shared<State>();
            restbed::Http::async(req,
                [this, filterChain, cb, pushNotifToken, state](const std::shared_ptr<restbed::Request>& req,
                                                               const std::shared_ptr<restbed::Response>& reply) {
                auto code = reply->get_status_code();
                if (code == 200) {
                    try {
                        std::string err;
                        Json::Value json;
                        Json::CharReaderBuilder rbuilder;
                        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
                        if (!deviceKey_.empty()) {
                            restbed::Http::fetch("\n", reply);
                            if (state->cancel)
                                return;
                            std::string body;
                            reply->get_body(body);

                            auto* char_data = reinterpret_cast<const char*>(&body[0]);
                            if (reader->parse(char_data, char_data + body.size(), &json, &err)) {
                                if (!json.isMember("token")) return;
                                *pushNotifToken = json["token"].asLargestUInt();
                            } else {
                                state->ok = false;
                            }
                        } else {
                            while (restbed::Http::is_open(req) and not state->cancel) {
                                restbed::Http::fetch("\n", reply);
                                if (state->cancel)
                                    break;
                                std::string body;
                                reply->get_body(body);
                                reply->set_body(""); // Reset the body for the next fetch

                                auto* char_data = reinterpret_cast<const char*>(&body[0]);
                                if (reader->parse(char_data, char_data + body.size(), &json, &err)) {
                                    auto value = std::make_shared<Value>(json);
                                    if ((not filterChain or filterChain(*value)) && cb)  {
                                        std::lock_guard<std::mutex> lock(lockCallbacks);
                                        callbacks_.emplace_back([cb, value, state]() {
                                            if (not state->cancel and not cb({value})) {
                                                state->cancel = true;
                                            }
                                        });
                                    }
                                } else {
                                    state->ok = false;
                                }
                            }
                        }
                    } catch (std::runtime_error&) {
                        state->ok = false;
                    }
                } else {
                    state->ok = false;
                }
            }, settings).get();
            if (not state->ok) {
                getConnectivityStatus();
            }
        }
    );
    {
        std::lock_guard<std::mutex> lock(lockListener_);
        listeners_.emplace_back(std::move(l));
    }
    return listener_token_;
}

bool
DhtProxyClient::cancelListen(const InfoHash&, size_t token)
{
    std::lock_guard<std::mutex> lock(lockListener_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        auto& listener = *it;
        if (listener.token == token) {
            if (!deviceKey_.empty()) {
                // First, be sure to have a token
                if (listener.thread.joinable()) {
                    listener.thread.join();
                }
                // UNSUBSCRIBE
                restbed::Uri uri(HTTP_PROTO + serverHost_ + "/" + listener.key);
                auto req = std::make_shared<restbed::Request>(uri);
                req->set_method("UNSUBSCRIBE");
                restbed::Http::async(req,
                    [](const std::shared_ptr<restbed::Request>&,
                       const std::shared_ptr<restbed::Response>&){}
                );
                // And remove
                listeners_.erase(it);
                return true;
            } else {
                // Just stop the request
                if (listener.thread.joinable()) {
                    // Close connection to stop listener?
                    if (listener.req)
                        restbed::Http::close(listener.req);
                    listener.thread.join();
                    listeners_.erase(it);
                    lockListener_.unlock();
                    return true;
                }
            }
        }
    }
    return false;
}

void
DhtProxyClient::getConnectivityStatus()
{
    auto proxyInfos = getProxyInfos();
    // NOTE: json["ipvX"] contains NodeStats::toJson()
    try {
        auto goodIpv4 = static_cast<long>(proxyInfos["ipv4"]["good"].asLargestUInt());
        auto dubiousIpv4 = static_cast<long>(proxyInfos["ipv4"]["dubious"].asLargestUInt());
        statusIpv4_ = (goodIpv4 + dubiousIpv4 > 0) ?  NodeStatus::Connected : NodeStatus::Disconnected;

        auto goodIpv6 = static_cast<long>(proxyInfos["ipv6"]["good"].asLargestUInt());
        auto dubiousIpv6 = static_cast<long>(proxyInfos["ipv6"]["dubious"].asLargestUInt());
        statusIpv6_ = (goodIpv6 + dubiousIpv6 > 0) ?  NodeStatus::Connected : NodeStatus::Disconnected;

        myid = InfoHash(proxyInfos["node_id"].asString());
        if (statusIpv4_ == NodeStatus::Disconnected && statusIpv6_ == NodeStatus::Disconnected) {
            const auto& now = scheduler.time();
            scheduler.edit(nextProxyConfirmation, now);
        }
    } catch (...) {
        statusIpv4_ = NodeStatus::Disconnected;
        statusIpv6_ = NodeStatus::Disconnected;
        const auto& now = scheduler.time();
        scheduler.edit(nextProxyConfirmation, now);
    }
}

void
DhtProxyClient::restartListeners()
{
    std::lock_guard<std::mutex> lock(lockListener_);
    for (auto& listener: listeners_) {
        if (listener.thread.joinable())
            listener.thread.join();
        // Redo listen
        auto filterChain = listener.filterChain;
        auto cb = listener.cb;
        restbed::Uri uri(HTTP_PROTO + serverHost_ + "/" + listener.key);
        auto req = std::make_shared<restbed::Request>(uri);
        req->set_method("LISTEN");
        listener.req = req;
        listener.thread = std::thread([this, filterChain, cb, req]()
            {
                auto settings = std::make_shared<restbed::Settings>();
                std::chrono::milliseconds timeout(std::numeric_limits<int>::max());
                settings->set_connection_timeout(timeout); // Avoid the client to close the socket after 5 seconds.

                auto ok = std::make_shared<std::atomic_bool>(true);
                restbed::Http::async(req,
                    [this, filterChain, cb, ok](const std::shared_ptr<restbed::Request>& req,
                                                const std::shared_ptr<restbed::Response>& reply) {
                    auto code = reply->get_status_code();

                    if (code == 200) {
                        try {
                            while (restbed::Http::is_open(req)) {
                                restbed::Http::fetch("\n", reply);
                                std::string body;
                                reply->get_body(body);
                                reply->set_body(""); // Reset the body for the next fetch

                                Json::Value json;
                                std::string err;
                                Json::CharReaderBuilder rbuilder;
                                auto* char_data = reinterpret_cast<const char*>(&body[0]);
                                auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
                                if (reader->parse(char_data, char_data + body.size(), &json, &err)) {
                                    auto value = std::make_shared<Value>(json);
                                    if ((not filterChain or filterChain(*value)) && cb) {
                                        auto okCb = std::make_shared<std::promise<bool>>();
                                        auto futureCb = okCb->get_future();
                                        {
                                            std::lock_guard<std::mutex> lock(lockCallbacks);
                                            callbacks_.emplace_back([cb, value, okCb](){
                                                okCb->set_value(cb({value}));
                                            });
                                        }
                                        futureCb.wait();
                                        if (!futureCb.get()) {
                                            return;
                                        }
                                    }
                                }
                            }
                        } catch (std::runtime_error&) {
                            // NOTE: Http::close() can occurs here. Ignore this.
                        }
                    } else {
                        *ok = false;
                    }
                }, settings).get();
                if (!ok) getConnectivityStatus();
            }
        );
    }
}

#if OPENDHT_PUSH_NOTIFICATIONS
void
DhtProxyClient::pushNotificationReceived(const Json::Value& notification)
{
    if (!notification.isMember("token")) return;
    auto token = notification["token"].asLargestUInt();
    // Find listener
    for (const auto& listener: listeners_)
        if (*(listener.pushNotifToken) == token) {
            if (notification.isMember("timeout")) {
                // A timeout has occured, we need to relaunch the listener
                resubscribe(token);
            } else {
                // Wake up daemon and get values
                get(InfoHash(listener.key), listener.cb, {}, listener.filterChain);
            }
        }
}

void
DhtProxyClient::resubscribe(const unsigned token)
{
    if (deviceKey_.empty()) return;
    for (auto& listener: listeners_) {
        if (*(listener.pushNotifToken) == token) {
            // Subscribe
            restbed::Uri uri(HTTP_PROTO + serverHost_ + "/" + listener.key);
            auto req = std::make_shared<restbed::Request>(uri);
            req->set_method("SUBSCRIBE");

            auto pushNotifToken = std::make_shared<unsigned>(0);

            if (listener.thread.joinable())
                listener.thread.join();
            listener.req = req;
            listener.pushNotifToken = pushNotifToken;
            listener.thread = std::thread([=]()
            {
                fillBodyToGetToken(req);
                auto settings = std::make_shared<restbed::Settings>();
                auto ok = std::make_shared<std::atomic_bool>(true);
                restbed::Http::async(req,
                    [this, pushNotifToken, ok](const std::shared_ptr<restbed::Request>&,
                                               const std::shared_ptr<restbed::Response>& reply) {
                    auto code = reply->get_status_code();
                    if (code == 200) {
                        try {
                            restbed::Http::fetch("\n", reply);
                            std::string body;
                            reply->get_body(body);

                            std::string err;
                            Json::Value json;
                            Json::CharReaderBuilder rbuilder;
                            auto* char_data = reinterpret_cast<const char*>(&body[0]);
                            auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
                            if (reader->parse(char_data, char_data + body.size(), &json, &err)) {
                                if (!json.isMember("token")) return;
                                *pushNotifToken = json["token"].asLargestUInt();
                            }
                        } catch (std::runtime_error&) {
                            // NOTE: Http::close() can occurs here. Ignore this.
                        }
                    } else {
                        *ok = false;
                    }
                }, settings).get();
                if (!ok) getConnectivityStatus();
            });
        }
    }
}

void
DhtProxyClient::fillBodyToGetToken(std::shared_ptr<restbed::Request> req)
{
    // Fill body with
    // {
    //   "key":"device_key",
    //   "callback_id": xxx
    // }
    Json::Value body;
    body["key"] = deviceKey_;
    {
        std::lock_guard<std::mutex> lock(lockCallback_);
        callbackId_ += 1;
        body["callback_id"] = callbackId_;
    }
#ifdef __ANDROID__
    body["isAndroid"] = true;
#endif
#ifdef __APPLE__
    body["isAndroid"] = false;
#endif
    Json::StreamWriterBuilder wbuilder;
    wbuilder["commentStyle"] = "None";
    wbuilder["indentation"] = "";
    auto content = Json::writeString(wbuilder, body) + "\n";
    std::replace(content.begin(), content.end(), '\n', ' ');
    req->set_body(content);
    req->set_header("Content-Length", std::to_string(content.size()));
}
#endif // OPENDHT_PUSH_NOTIFICATIONS

} // namespace dht

#endif // OPENDHT_PROXY_CLIENT
