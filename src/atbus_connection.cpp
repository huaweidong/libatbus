﻿#include <cstdio>
#include <assert.h>
#include <ctime>
#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <cstdlib>

#include "common/string_oprs.h"

#include "detail/buffer.h"

#include "atbus_node.h"
#include "atbus_connection.h"

#include "detail/libatbus_protocol.h"

namespace atbus {
    namespace detail {
        struct connection_async_data {
            node* owner_node;
            connection::ptr_t conn;
            
            connection_async_data(node* o): owner_node(o) {
                assert(owner_node);
                owner_node->ref_object(reinterpret_cast<void*>(this));
            }
            
            ~connection_async_data() {
                owner_node->unref_object(reinterpret_cast<void*>(this));
            }
            
            connection_async_data(const connection_async_data& other): owner_node(other.owner_node), conn(other.conn) {
                assert(owner_node);
                owner_node->ref_object(reinterpret_cast<void*>(this));
            }
            
            connection_async_data& operator=(const connection_async_data& other) {
                assert(owner_node);
                assert(other.owner_node);
                
                if (owner_node != other.owner_node) {
                    owner_node->unref_object(reinterpret_cast<void*>(this));
                    other.owner_node->ref_object(reinterpret_cast<void*>(this));
                    
                    owner_node = other.owner_node;
                }
                
                conn = other.conn;
            }
        };
    }

    connection::connection():state_(state_t::DISCONNECTED), owner_(NULL), binding_(NULL){
        flags_.reset();
        memset(&conn_data_, 0, sizeof(conn_data_));
    }

    connection::ptr_t connection::create(node* owner) {
        if (!owner) {
            return connection::ptr_t();
        }

        connection::ptr_t ret(new connection());
        if (!ret) {
            return ret;
        }

        ret->owner_ = owner;
        ret->watcher_ = ret;

        owner->add_connection_timer(ret);
        return ret;
    }

    connection::~connection() {
        flags_.set(flag_t::DESTRUCTING, true);

        reset();
    }

    void connection::reset() {
        // 这个函数可能会在析构时被调用，这时候不能使用watcher_.lock()
        if(flags_.test(flag_t::RESETTING)) {
            return;
        }
        flags_.set(flag_t::RESETTING, true);

        // 需要临时给自身加引用计数，否则后续移除的过程中可能导致数据被提前释放
        ptr_t tmp_holder = watcher_.lock();

        disconnect();

        if (NULL != binding_) {
            binding_->remove_connection(this);

            // 只能由上层设置binding_所属的节点
            // binding_ = NULL;
            assert(NULL == binding_);
        }

        flags_.reset();
        // 只要connection存在，则它一定存在于owner_的某个位置。
        // 并且这个值只能在创建时指定，所以不能重置这个值
        // owner_ = NULL;
    }

    int connection::proc(node& n, time_t sec, time_t usec) {
        if (state_t::CONNECTED != state_) {
            return 0;
        }

        if (NULL != conn_data_.proc_fn) {
            return conn_data_.proc_fn(n, *this, sec, usec);
        }

        return 0;
    }

    int connection::listen(const char* addr_str) {
        if (state_t::DISCONNECTED != state_) {
            return EN_ATBUS_ERR_ALREADY_INITED;
        }

        if (NULL == owner_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }
        const node::conf_t& conf = owner_->get_conf();

        if (false == channel::make_address(addr_str, address_)) {
            return EN_ATBUS_ERR_CHANNEL_ADDR_INVALID;
        }

        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem", address_.scheme.c_str(), 3)) {
            channel::mem_channel* mem_chann = NULL;
            intptr_t ad;
            util::string::str2int(ad, address_.host.c_str());
            int res = channel::mem_attach(reinterpret_cast<void*>(ad), conf.recv_buffer_size, &mem_chann, NULL);
            if (res < 0) {
                res = channel::mem_init(reinterpret_cast<void*>(ad), conf.recv_buffer_size, &mem_chann, NULL);
            }

            if (res < 0) {
                return res;
            }
            
            conn_data_.proc_fn = mem_proc_fn;
            conn_data_.free_fn = mem_free_fn;

            // 加入轮询队列
            conn_data_.shared.mem.channel = mem_chann;
            conn_data_.shared.mem.buffer = reinterpret_cast<void*>(ad);
            conn_data_.shared.mem.len = conf.recv_buffer_size;
            owner_->add_proc_connection(watcher_.lock());
            flags_.set(flag_t::REG_PROC, true);
            flags_.set(flag_t::ACCESS_SHARE_ADDR, true);
            flags_.set(flag_t::ACCESS_SHARE_HOST, true);
            state_ = state_t::CONNECTED;

            return res;
        } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm", address_.scheme.c_str(), 3)) {
            channel::shm_channel* shm_chann = NULL;
            key_t shm_key;
            util::string::str2int(shm_key, address_.host.c_str());
            int res = channel::shm_attach(shm_key, conf.recv_buffer_size, &shm_chann, NULL);
            if (res < 0) {
                res = channel::shm_init(shm_key, conf.recv_buffer_size, &shm_chann, NULL);
            }

            if (res < 0) {
                return res;
            }

            conn_data_.proc_fn = shm_proc_fn;
            conn_data_.free_fn = shm_free_fn;

            // 加入轮询队列
            conn_data_.shared.shm.channel = shm_chann;
            conn_data_.shared.shm.shm_key = shm_key;
            conn_data_.shared.shm.len = conf.recv_buffer_size;
            owner_->add_proc_connection(watcher_.lock());
            flags_.set(flag_t::REG_PROC, true);
            flags_.set(flag_t::ACCESS_SHARE_HOST, true);
            state_ = state_t::CONNECTED;

            return res;
        } else {
            detail::connection_async_data* async_data = new detail::connection_async_data(owner_);
            if (NULL == async_data) {
                return EN_ATBUS_ERR_MALLOC;
            }
            connection::ptr_t self = watcher_.lock();
            async_data->conn = self;

            state_ = state_t::CONNECTING;
            int res = channel::io_stream_listen(owner_->get_iostream_channel(), address_, iostream_on_listen_cb, async_data, 0);
            if (res < 0) {
                delete async_data;
                return res;
            }
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    int connection::connect(const char* addr_str) {
        if (state_t::DISCONNECTED != state_) {
            return EN_ATBUS_ERR_ALREADY_INITED;
        }

        if (NULL == owner_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }
        const node::conf_t& conf = owner_->get_conf();

        if (false == channel::make_address(addr_str, address_)) {
            return EN_ATBUS_ERR_CHANNEL_ADDR_INVALID;
        }

        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem", address_.scheme.c_str(), 3)) {
            channel::mem_channel* mem_chann = NULL;
            intptr_t ad;
            util::string::str2int(ad, address_.host.c_str());
            int res = channel::mem_attach(reinterpret_cast<void*>(ad), conf.recv_buffer_size, &mem_chann, NULL);
            if (res < 0) {
                res = channel::mem_init(reinterpret_cast<void*>(ad), conf.recv_buffer_size, &mem_chann, NULL);
            }

            if (res < 0) {
                return res;
            }

            conn_data_.proc_fn = mem_proc_fn;
            conn_data_.free_fn = mem_free_fn;
            conn_data_.push_fn = mem_push_fn;

            // 连接信息
            conn_data_.shared.mem.channel = mem_chann;
            conn_data_.shared.mem.buffer = reinterpret_cast<void*>(ad);
            conn_data_.shared.mem.len = conf.recv_buffer_size;
            flags_.set(flag_t::REG_PROC, true);
            if (NULL == binding_) {
                state_ = state_t::HANDSHAKING;
            } else {
                state_ = state_t::CONNECTED;
            }

            return res;
        } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm", address_.scheme.c_str(), 3)) {
            channel::shm_channel* shm_chann = NULL;
            key_t shm_key;
            util::string::str2int(shm_key, address_.host.c_str());
            int res = channel::shm_attach(shm_key, conf.recv_buffer_size, &shm_chann, NULL);
            if (res < 0) {
                res = channel::shm_init(shm_key, conf.recv_buffer_size, &shm_chann, NULL);
            }

            if (res < 0) {
                return res;
            }

            conn_data_.proc_fn = shm_proc_fn;
            conn_data_.free_fn = shm_free_fn;
            conn_data_.push_fn = shm_push_fn;

            // 连接信息
            conn_data_.shared.shm.channel = shm_chann;
            conn_data_.shared.shm.shm_key = shm_key;
            conn_data_.shared.shm.len = conf.recv_buffer_size;

            flags_.set(flag_t::REG_PROC, true);
            if (NULL == binding_) {
                state_ = state_t::HANDSHAKING;
            } else {
                state_ = state_t::CONNECTED;
            }

            return res;
        } else {
            detail::connection_async_data* async_data = new detail::connection_async_data(owner_);
            if (NULL == async_data) {
                return EN_ATBUS_ERR_MALLOC;
            }
            connection::ptr_t self = watcher_.lock();
            async_data->conn = self;

            state_ = state_t::CONNECTING;
            int res = channel::io_stream_connect(owner_->get_iostream_channel(), address_, iostream_on_connected_cb, async_data, 0);
            if (res < 0) {
                delete async_data;
                return res;
            }
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    int connection::disconnect() {
        if (state_t::DISCONNECTED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        if (state_t::DISCONNECTING == state_) {
            return EN_ATBUS_ERR_SUCCESS;
        }

        state_ = state_t::DISCONNECTING;
        if (NULL != conn_data_.free_fn) {
            if (NULL != owner_) {
                int res = conn_data_.free_fn(*owner_, *this);
                if (res < 0) {
                    ATBUS_FUNC_NODE_DEBUG(*owner_, get_binding(), this, "destroy connection failed, res: %d", res);
                }
            }
        }

        if (NULL != owner_) {
            ATBUS_FUNC_NODE_DEBUG(*owner_, get_binding(), this, "connection disconnected");
            owner_->on_disconnect(this);
        }

        // 移除proc队列
        if (flags_.test(flag_t::REG_PROC)) {
            if (NULL != owner_) {
                owner_->remove_proc_connection(address_.address);
            }
            flags_.set(flag_t::REG_PROC, false);
        }

        memset(&conn_data_, 0, sizeof(conn_data_));
        state_ = state_t::DISCONNECTED;
        return EN_ATBUS_ERR_SUCCESS;
    }

    int connection::push(const void* buffer, size_t s) {
        if (state_t::CONNECTED != state_ && state_t::HANDSHAKING != state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        if (NULL == conn_data_.push_fn) {
            return EN_ATBUS_ERR_ACCESS_DENY;
        }

        return conn_data_.push_fn(*this, buffer, s);
    }

    bool connection::is_connected() const {
        return state_t::CONNECTED == state_;
    }

    endpoint* connection::get_binding() {
        return binding_;
    }

    const endpoint* connection::get_binding() const {
        return binding_;
    }

    connection::ptr_t connection::watch() const {
        if (flags_.test(flag_t::DESTRUCTING) || watcher_.expired()) {
            return connection::ptr_t();
        }

        return watcher_.lock();
    }


    /** 是否正在连接、或者握手或者已连接 **/
    bool connection::is_running() const {
        return state_t::CONNECTING == state_ || state_t::HANDSHAKING == state_ || state_t::CONNECTED == state_;
    }

    void connection::iostream_on_listen_cb(channel::io_stream_channel* channel, channel::io_stream_connection* connection, int status, void* buffer, size_t s) {
        detail::connection_async_data* async_data = reinterpret_cast<detail::connection_async_data*>(buffer);
        assert(NULL != async_data);
        if(NULL == async_data) {
            return;
        }

        if (status < 0) {
            ATBUS_FUNC_NODE_ERROR(*async_data->owner_node, async_data->conn->binding_, async_data->conn.get(), status, channel->error_code);
            async_data->conn->state_ = state_t::DISCONNECTED;

        } else {
            async_data->conn->flags_.set(flag_t::REG_FD, true);
            async_data->conn->state_ = state_t::CONNECTED;

            async_data->conn->conn_data_.shared.ios_fd.channel = channel;
            async_data->conn->conn_data_.shared.ios_fd.conn = connection;
            async_data->conn->conn_data_.free_fn = ios_free_fn;
            connection->data = async_data->conn.get();
        }

        delete async_data;
    }

    void connection::iostream_on_connected_cb(channel::io_stream_channel* channel, channel::io_stream_connection* connection, int status, void* buffer, size_t s) {
        detail::connection_async_data* async_data = reinterpret_cast<detail::connection_async_data*>(buffer);
        assert(NULL != async_data);
        if (NULL == async_data) {
            return;
        }

        if (status < 0) {
            ATBUS_FUNC_NODE_ERROR(*async_data->owner_node, async_data->conn->binding_, async_data->conn.get(), status, channel->error_code);
            async_data->conn->state_ = state_t::DISCONNECTED;

        } else {
            async_data->conn->flags_.set(flag_t::REG_FD, true);
            if (NULL == async_data->conn->binding_) {
                async_data->conn->state_ = state_t::HANDSHAKING;
            } else {
                async_data->conn->state_ = state_t::CONNECTED;
            }

            async_data->conn->conn_data_.shared.ios_fd.channel = channel;
            async_data->conn->conn_data_.shared.ios_fd.conn = connection;

            async_data->conn->conn_data_.free_fn = ios_free_fn;
            async_data->conn->conn_data_.push_fn = ios_push_fn;
            connection->data = async_data->conn.get();


            ATBUS_FUNC_NODE_DEBUG(*async_data->owner_node, NULL, async_data->conn.get(), "finish a new connection");
            async_data->owner_node->on_new_connection(async_data->conn.get());
        }

        delete async_data;
    }

    void connection::iostream_on_recv_cb(channel::io_stream_channel* channel, channel::io_stream_connection* conn_ios, int status, void* buffer, size_t s) {

        assert(channel && channel->data);
        node* _this = reinterpret_cast<node*>(channel->data);
        connection* conn = reinterpret_cast<connection*>(conn_ios->data);

        if (status < 0 || NULL == buffer || s <= 0) {
            _this->on_recv(conn, NULL, status, channel->error_code);
            return;
        }

        // connection 已经释放并解除绑定，这时候会先把剩下未处理的消息处理完再关闭
        if (NULL == conn) {
            //ATBUS_FUNC_NODE_ERROR(*_this, NULL, conn, EN_ATBUS_ERR_UNPACK, EN_ATBUS_ERR_PARAMS);
            return;
        }

        // unpack
        msgpack::unpacked result;
        protocol::msg m;
        if (false == unpack(&result, *conn, m, buffer, s)) {
            return;
        }
        _this->on_recv(conn, &m, status, channel->error_code);
    }

    void connection::iostream_on_accepted(channel::io_stream_channel* channel, channel::io_stream_connection* conn_ios, int status, void* buffer, size_t s) {
        // 连接成功加入点对点传输池
        // 加入超时检测
        node* n = reinterpret_cast<node*>(channel->data);
        assert(NULL != n);
        if (NULL == n) {
            channel::io_stream_disconnect(channel, conn_ios, NULL);
            return;
        }

        ptr_t conn = create(n);
        conn->state_ = state_t::HANDSHAKING;
        conn->flags_.set(flag_t::REG_FD, true);

        conn->conn_data_.free_fn = ios_free_fn;
        conn->conn_data_.push_fn = ios_push_fn;

        conn->conn_data_.shared.ios_fd.channel = channel;
        conn->conn_data_.shared.ios_fd.conn = conn_ios;
        conn_ios->data = conn.get();


        ATBUS_FUNC_NODE_DEBUG(*n, NULL, conn.get(), "accept a new connection");
        n->on_new_connection(conn.get());
    }

    void connection::iostream_on_connected(channel::io_stream_channel* channel, channel::io_stream_connection* conn_ios, int status, void* buffer, size_t s) {
    }

    void connection::iostream_on_disconnected(channel::io_stream_channel* channel, channel::io_stream_connection* conn_ios, int status, void* buffer, size_t s) {
        connection* conn = reinterpret_cast<connection*>(conn_ios->data);
        
        // 主动关闭时会先释放connection，这时候connection已经被释放，不需要再重置
        if (NULL == conn) {
            return;
        }

        ATBUS_FUNC_NODE_DEBUG(*conn->owner_, conn->get_binding(), conn, "connection reset by peer");
        conn->reset();
    }

    int connection::shm_proc_fn(node& n, connection& conn, time_t sec, time_t usec) {
        int ret = 0;
        size_t left_times = n.get_conf().loop_times;
        detail::buffer_block* static_buffer = n.get_temp_static_buffer();
        if (NULL == static_buffer) {
            return ATBUS_FUNC_NODE_ERROR(n, NULL, &conn, EN_ATBUS_ERR_NOT_INITED, 0);
        }

        while (left_times-- > 0) {
            size_t recv_len;
            int res = channel::shm_recv(
                conn.conn_data_.shared.shm.channel,
                static_buffer->data(),
                static_buffer->size(),
                &recv_len
            );

            if (EN_ATBUS_ERR_NO_DATA == res) {
                break;
            }

            // 回调收到数据事件
            if (res < 0) {
                ret = res;
                n.on_recv(&conn, NULL, res, res);
                break;
            } else {
                // unpack
                msgpack::unpacked result;
                protocol::msg m;
                if (false == unpack(&result, conn, m, static_buffer->data(), recv_len)) {
                    continue;
                }

                n.on_recv(&conn, &m, res, res);
                ++ret;
            }
        }

        return ret;
    }

    int connection::shm_free_fn(node& n, connection& conn) {
        return channel::shm_close(conn.conn_data_.shared.shm.shm_key);
    }

    int connection::shm_push_fn(connection& conn, const void* buffer, size_t s) {
        return channel::shm_send(conn.conn_data_.shared.shm.channel, buffer, s);
    }

    int connection::mem_proc_fn(node& n, connection& conn, time_t sec, time_t usec) {
        int ret = 0;
        size_t left_times = n.get_conf().loop_times;
        detail::buffer_block* static_buffer = n.get_temp_static_buffer();
        if (NULL == static_buffer) {
            return ATBUS_FUNC_NODE_ERROR(n, NULL, &conn, EN_ATBUS_ERR_NOT_INITED, 0);
        }

        while (left_times-- > 0) {
            size_t recv_len;
            int res = channel::mem_recv(
                conn.conn_data_.shared.mem.channel,
                static_buffer->data(),
                static_buffer->size(),
                &recv_len
            );

            if (EN_ATBUS_ERR_NO_DATA == res) {
                break;
            }

            // 回调收到数据事件
            if (res < 0) {
                ret = res;
                n.on_recv(&conn, NULL, res, res);
                break;
            } else {
                // unpack
                msgpack::unpacked result;
                protocol::msg m;
                if (false == unpack(&result, conn, m, static_buffer->data(), recv_len)) {
                    continue;
                }

                n.on_recv(&conn, &m, res, res);
                ++ret;
            }
        }

        return ret;
    }

    int connection::mem_free_fn(node& n, connection& conn) {
        return 0;
    }

    int connection::mem_push_fn(connection& conn, const void* buffer, size_t s) {
        return channel::mem_send(conn.conn_data_.shared.mem.channel, buffer, s);
    }

    int connection::ios_free_fn(node& n, connection& conn) {
        int ret = channel::io_stream_disconnect(conn.conn_data_.shared.ios_fd.channel, conn.conn_data_.shared.ios_fd.conn, NULL);
        // 释放后移除关联关系
        conn.conn_data_.shared.ios_fd.conn->data = NULL;

        return ret;
    }

    int connection::ios_push_fn(connection& conn, const void* buffer, size_t s) {
        return channel::io_stream_send(conn.conn_data_.shared.ios_fd.conn, buffer, s);
    }

    bool connection::unpack(void* res, connection& conn, atbus::protocol::msg& m, void* buffer, size_t s) {
        msgpack::unpacked* result = reinterpret_cast<msgpack::unpacked*>(res);
        msgpack::unpack(*result, reinterpret_cast<const char*>(buffer), s);
        msgpack::object obj = result->get();
        if (obj.is_nil()) {
            ATBUS_FUNC_NODE_ERROR(*conn.owner_, conn.binding_, &conn, EN_ATBUS_ERR_UNPACK, EN_ATBUS_ERR_UNPACK);
            return false;
        }

        obj.convert(m);
        return true;
    }
}
