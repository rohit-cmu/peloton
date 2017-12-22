//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// connection_handle_factory.h
//
// Identification: src/include/network/connection_handle_factory.h
//
// Copyright (c) 2015-17, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include "network/connection_handle.h"
#include "peloton_server.h"

namespace peloton {
namespace network {

/**
 * @brief Factory class for constructing ConnectionHandles
 * The rationale behind using a factory is that buffers are expensive to allocate and allocating new
 * ones every time is a bottleneck for throughput.
 */
class ConnectionHandleFactory {
public:
  /**
   * Creates or repurpose a ConnectionHandle to be run on the given handler, handling connection from conn_fd
   * @param conn_fd Client connection fd.
   * @param handler The handler this ConnectionHandle is assigned to
   * @return
   */
  std::shared_ptr<ConnectionHandle> GetConnectionHandle(int conn_fd, ConnectionHandlerTask *handler) {
    // TODO(tianyu): The use of a static variable here for testing purpose is VILE. Fix this in a later refactor
    // (probably also to-do: beat up the person who wrote this)
    PelotonServer::recent_connfd = conn_fd;
    auto it = reusable_handles_.find(conn_fd);
    if (it == reusable_handles_.end()){
      // We are not using std::make_shared here because we want to keep ConnectionHandle constructor
      // private to avoid unintentional use.
      auto handle = std::shared_ptr<ConnectionHandle>(new ConnectionHandle(conn_fd, handler));
      reusable_handles_[conn_fd] = handle;
      return handle;
    }
    return Reset(it->second);
  }

  // TODO(tianyu) Again, this is VILE. Fix this in a later refactor.
  /**
   * Exposed for testing only. DO NOT USE ELSEWHERE IN CODE.
   * @param conn_fd client socket fd
   * @return ConnetionHandle object representing client connection at conn_fd
   */
  std::shared_ptr<ConnectionHandle> ConnectionHandleAt(int conn_fd) {
    return reusable_handles_[conn_fd];
  }

  // TODO(tianyu): This should removed with the rest of the singletons
  // We are keeping this here as fixing singleton is not the focus of this
  // refactor and fixing it would be pretty expensive.
  static ConnectionHandleFactory &GetInstance() {
    static ConnectionHandleFactory factory;
    return factory;
  }

private:

  std::shared_ptr<ConnectionHandle> &Reset(std::shared_ptr<ConnectionHandle> &handle) {
    handle->client_.Reset();
    handle->rbuf_.Reset();
    handle->wbuf_.Reset();
    handle->protocol_handler_->Reset();
    handle->traffic_cop_.Reset();
    handle->next_response_ = 0;
    handle->ssl_sent_ = false;
    if (handle->network_event != nullptr)
      handle->handler_->UnregisterEvent(handle->network_event);
    if (handle->workpool_event != nullptr)
      handle->handler_->UnregisterEvent(handle->workpool_event);
    return handle;
  }

  std::unordered_map<int, std::shared_ptr<ConnectionHandle>> reusable_handles_;
};
  
}
}
