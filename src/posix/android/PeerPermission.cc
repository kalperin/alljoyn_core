/**
 * @file
 * This class is to impose permission verification on a peer when it tries to invoke a method/signal call
 * This only applies to the scenario that two peers on the same device connecting to the same pre-installed alljoyn daemon.
 */

/******************************************************************************
 * Copyright 2012, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/
#include "PermissionDB.h"
#include "PeerPermission.h"
#include <qcc/String.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <alljoyn/DBusStd.h>
#include <map>

using namespace qcc;

#define QCC_MODULE "PEER_PERMISSION"

namespace ajn {

/**
 * Type definition for a permission-checked method or signal call.
 */
typedef struct PermCheckedEntry {
  public:
    const qcc::String sender;                            /**< The endpoint name that issues the call */
    const qcc::String sourcePath;                        /**< The object path of the call */
    const qcc::String iface;                             /**< The interface name of the call */
    const qcc::String signalName;                        /**< The method or signal name of the call */
    PermCheckedEntry(const qcc::String& sender, const qcc::String& sourcePath, const qcc::String& iface, const qcc::String& signalName) : sender(sender), sourcePath(sourcePath), iface(iface), signalName(signalName) { }
    bool operator<(const PermCheckedEntry& other) const {
        return (sender < other.sender) || ((sender == other.sender) && (sourcePath < other.sourcePath))
               || ((sourcePath == other.sourcePath) && (iface < other.iface))
               || ((iface == other.iface) && (signalName < other.signalName));
    }
} PermCheckedEntry;

std::map<PermCheckedEntry, bool> permCheckedCallMap;     /**< Map of a permission-checked method/signal call to the verification result */
qcc::Mutex chkedCallMapLock;                             /**< Mutex protecting the map of verification result */

PeerPermission::PeerPermStatus DoPeerPermissionInquiry(LocalEndpoint* localEp, Message& message, qcc::String& permsStr)
{
    QStatus status = ER_OK;
    /* Split permissions that are concated by ";". The permission string is in form of "PERM0;PERM1;..." */
    std::set<qcc::String> permsReq;
    size_t pos;
    while ((pos = permsStr.find_first_of(";")) != String::npos) {
        qcc::String tmp = permsStr.substr(0, pos);
        permsReq.insert(tmp);
        permsStr.erase(0, pos + 1);
    }
    if (permsStr.size() > 0) {
        permsReq.insert(permsStr);
    }

    bool allowed = true;
    uint32_t userId = -1;
    /* Inquire daemon about the user id of the sender */
    MsgArg arg("s", message->GetSender());
    Message reply(localEp->GetBus());
    status = localEp->GetDBusProxyObj().MethodCall(org::freedesktop::DBus::InterfaceName,
                                                   "GetConnectionUnixUser",
                                                   &arg,
                                                   1,
                                                   reply);

    if (status == ER_OK) {
        userId = reply->GetArg(0)->v_uint32;
    }

    /* The permission check is only required for UnixEndpoint */
    if (userId != (uint32_t)-1) {
        allowed = PermissionDB::GetDB().VerifyPeerPermissions(userId, permsReq);
    }

    QCC_DbgPrintf(("VerifyPeerPermissions result: allowed = %d", allowed));
    chkedCallMapLock.Lock(MUTEX_CONTEXT);
    /* Be defensive. Limit the map cache size to be no more than MAX_PERM_CHECKEDCALL_SIZE */
    if (permCheckedCallMap.size() > MAX_PERM_CHECKEDCALL_SIZE) {
        permCheckedCallMap.clear();
    }
    PermCheckedEntry permChkEntry(message->GetSender(), message->GetObjectPath(), message->GetInterface(), message->GetMemberName());
    permCheckedCallMap[permChkEntry] = allowed;  /* Cache the result */
    chkedCallMapLock.Unlock(MUTEX_CONTEXT);
    if (allowed) {
        return PeerPermission::PP_ALLOWED;
    } else {
        return PeerPermission::PP_DENIED;
    }
}

PeerPermission::PeerPermStatus PeerPermission::CanPeerDoCall(Message& message, const qcc::String& permStr)
{
    QCC_DbgPrintf(("PeerPermission::CanPeerDoCall (%s::%s) requires peer permission %s", message->GetInterface(), message->GetMemberName(), permStr.c_str()));
    PeerPermission::PeerPermStatus pps = PeerPermission::PP_PENDING;
    chkedCallMapLock.Lock(MUTEX_CONTEXT);
    PermCheckedEntry permChkEntry(message->GetSender(), message->GetObjectPath(), message->GetInterface(), message->GetMemberName());
    std::map<PermCheckedEntry, bool>::const_iterator it = permCheckedCallMap.find(permChkEntry);
    if (it != permCheckedCallMap.end()) {
        if (!permCheckedCallMap[permChkEntry]) {
            pps = PP_ALLOWED;
        } else {
            pps = PP_DENIED;
        }
    }
    chkedCallMapLock.Unlock(MUTEX_CONTEXT);
    return pps;
}

class MethodCallRunnableAuth : public Runnable {
  public:
    MethodCallRunnableAuth(LocalEndpoint* localEp, const MethodTable::Entry* entry, Message& message, qcc::String& permStr)
        : localEp(localEp), entry(entry), message(message), permStr(permStr)
    {
        QCC_DbgHLPrintf(("MethodCallRunnableAuth::MethodCallRunnable(): New closure for method call"));
    }

    virtual void Run(void)
    {
        QCC_DbgHLPrintf(("MethodCallRunnableAuth::Run(): Firing closure for method call"));
        PeerPermission::PeerPermStatus pps = DoPeerPermissionInquiry(localEp, message, permStr);
        if (pps == PeerPermission::PP_ALLOWED) {
            localEp->DoCallMethodHandler(entry, message);
        } else {
            QCC_LogError(ER_ALLJOYN_ACCESS_PERMISSION_ERROR, ("Endpoint(%s) has no permission to call method (%s::%s)",
                                                              message->GetSender(), message->GetInterface(), message->GetMemberName()));
            if (!(message->GetFlags() & ALLJOYN_FLAG_NO_REPLY_EXPECTED)) {
                qcc::String errStr;
                qcc::String errMsg;
                errStr += "org.alljoyn.Bus.";
                errStr += QCC_StatusText(ER_ALLJOYN_ACCESS_PERMISSION_ERROR);
                errMsg = message->Description();
                localEp->SendErrMessage(message, errStr, errMsg);
            }
        }
    }

  private:
    LocalEndpoint* localEp;
    const MethodTable::Entry* entry;
    Message message;
    qcc::String permStr;
};

class SignalCallRunnableAuth : public Runnable {
  public:
    SignalCallRunnableAuth(MessageReceiver* object,
                           MessageReceiver::SignalHandler handler,
                           const InterfaceDescription::Member* member,
                           LocalEndpoint* localEp,
                           Message message,
                           qcc::String& permStr)
        : object(object), handler(handler), member(member), message(message), localEp(localEp), permStr(permStr)
    {
        QCC_DbgHLPrintf(("SignalCallRunnableAuth::SignalCallRunnableAuth(): New closure for signal call"));
    }

    virtual void Run(void)
    {
        QCC_DbgHLPrintf(("SignalCallRunnableAuth::Run(): Firing closure for signal call"));
        PeerPermission::PeerPermStatus pps = DoPeerPermissionInquiry(localEp, message, permStr);
        if (pps == PeerPermission::PP_ALLOWED) {
            (object->*handler)(member, message->GetObjectPath(), message);
        } else {
            QCC_LogError(ER_ALLJOYN_ACCESS_PERMISSION_ERROR, ("Endpoint(%s) has no permission to call signal (%s::%s). No ErrMsg returned.",
                                                              message->GetSender(), message->GetInterface(), message->GetMemberName()));
        }
    }

  private:
    MessageReceiver* object;
    MessageReceiver::SignalHandler handler;
    const InterfaceDescription::Member* member;
    Message message;
    LocalEndpoint* localEp;
    qcc::String permStr;

};

QStatus PeerPermission::PeerAuthAndHandleMethodCall(Message& message, LocalEndpoint* localEp, const MethodTable::Entry* entry, ThreadPool* threadPool, const qcc::String& permStr)
{
    QCC_DbgHLPrintf(("PeerPermission::PeerAuthAndHandleMethodCall(permStr=%s), permStr.c_str()"));
    QStatus status = ER_OK;
    Ptr<MethodCallRunnableAuth> runnable = NewPtr<MethodCallRunnableAuth>(localEp, entry, message, permStr);
    for (;;) {
        status = threadPool->WaitForAvailableThread();
        if (status != ER_OK) {
            break;
        }

        status = threadPool->Execute(runnable);
        if (status == ER_THREADPOOL_EXHAUSTED) {
            continue;
        } else {
            break;
        }
    }
    return status;
}

QStatus PeerPermission::PeerAuthAndHandleSignalCall(Message& message, LocalEndpoint* localEp, std::list<SignalTable::Entry>& callList, ThreadPool* threadPool, const qcc::String& permStr)
{
    QCC_DbgHLPrintf(("PeerPermission::PeerAuthAndHandleSignalCall(permStr=%s), permStr.c_str()"));
    QStatus status = ER_OK;
    std::list<SignalTable::Entry>::const_iterator callit;
    for (callit = callList.begin(); callit != callList.end(); ++callit) {
        /* Don't multithread signals originating locally.  See comment in similar code in MethodCallHandler */
        Ptr<SignalCallRunnableAuth> runnable = NewPtr<SignalCallRunnableAuth>(callit->object,
                                                                              callit->handler,
                                                                              callit->member,
                                                                              localEp,
                                                                              message,
                                                                              permStr);
        for (;;) {
            status = threadPool->WaitForAvailableThread();
            if (status != ER_OK) {
                break;
            }

            status = threadPool->Execute(runnable);
            if (status == ER_THREADPOOL_EXHAUSTED) {
                continue;
            } else {
                break;
            }
        }
    }
    return status;
}


} // namespace ajn {
