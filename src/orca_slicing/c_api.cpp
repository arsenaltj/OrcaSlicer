#include "include/orca/slicing_c.h"
#include "engine.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>
#ifndef ORCA_SLICING_SOURCE_COMMIT
#define ORCA_SLICING_SOURCE_COMMIT "unknown"
#endif
namespace {
struct Context { explicit Context(const char* root):engine(root){} orca::slicing::Engine engine; std::atomic<bool> busy{false}; };
std::mutex registry_mutex;
std::unordered_map<uintptr_t,std::shared_ptr<Context>> registry;
std::atomic<uintptr_t> next_handle{1};
std::shared_ptr<Context> acquire(orca_slicing_context* handle){std::lock_guard<std::mutex> lock(registry_mutex);auto it=registry.find(reinterpret_cast<uintptr_t>(handle));return it==registry.end()?nullptr:it->second;}
char* copy(const std::string& text){auto* p=static_cast<char*>(std::malloc(text.size()+1));if(p)std::memcpy(p,text.c_str(),text.size()+1);return p;}
int failure(int code,const char* name,const std::string& message,char** result){if(result)*result=copy(orca::slicing::Json{{"ok",false},{"error",{{"code",name},{"message",message}}}}.dump());return code;}
}
extern "C" {
const char* orca_slicing_version(){return "orca-slicing-c/1.0.0";}
const char* orca_slicing_capabilities(){return "{\"available\":true,\"abiVersion\":\"1.0\",\"engineVersion\":\"orca-slicing-c/1.0.0\",\"sourceCommit\":\"" ORCA_SLICING_SOURCE_COMMIT "\",\"formats\":[\"stl\",\"obj\",\"3mf\"],\"qualities\":[\"standard\",\"fine\",\"fast\",\"smart\"],\"nativeSlicing\":true,\"nativeRecommendation\":true,\"remoteAI\":false}";}
orca_slicing_context* orca_slicing_create(const char* workspace){if(!workspace||!*workspace)return nullptr;try{auto context=std::make_shared<Context>(workspace);auto id=next_handle.fetch_add(1);std::lock_guard<std::mutex> lock(registry_mutex);registry.emplace(id,std::move(context));return reinterpret_cast<orca_slicing_context*>(id);}catch(...){return nullptr;}}
int orca_slicing_execute(orca_slicing_context* handle,const char* request,orca_slicing_event_fn event,void* user,char** result){
 if(!result)return ORCA_SLICE_INVALID;*result=nullptr;auto context=acquire(handle);if(!context||!request)return failure(ORCA_SLICE_INVALID,"INVALID_ARGUMENT","Invalid slicing handle/request",result);
 bool expected=false;if(!context->busy.compare_exchange_strong(expected,true))return failure(ORCA_SLICE_BUSY,"BUSY","Slicing context already has an active operation",result);
 struct Reset{std::atomic<bool>& flag;~Reset(){flag.store(false);}} reset{context->busy};
 try{auto req=orca::slicing::Json::parse(request);auto answer=context->engine.execute(req,[&](const orca::slicing::Json& e){if(event){auto text=e.dump();event(text.c_str(),user);}});answer["ok"]=true;answer["engineVersion"]=orca_slicing_version();answer["sourceCommit"]=ORCA_SLICING_SOURCE_COMMIT;*result=copy(answer.dump());if(!*result)return ORCA_SLICE_RESOURCE;return ORCA_SLICE_OK;}
 catch(const nlohmann::json::exception& e){return failure(ORCA_SLICE_INVALID,"INVALID_JSON",e.what(),result);}
 catch(const std::bad_alloc&){return failure(ORCA_SLICE_RESOURCE,"MEMORY_LIMIT","Native allocation failed; project is preserved",result);}
 catch(const std::exception& e){const std::string msg=e.what();if(msg=="SLICING_CANCELED")return failure(ORCA_SLICE_CANCELED,"CANCELED",msg,result);return failure(ORCA_SLICE_INTERNAL,"SLICING_FAILED",msg,result);}
 catch(...){return failure(ORCA_SLICE_INTERNAL,"NATIVE_FAILURE","Unknown native slicing failure",result);}
}
void orca_slicing_cancel(orca_slicing_context* handle){if(auto c=acquire(handle))c->engine.cancel();}
void orca_slicing_destroy(orca_slicing_context* handle){std::shared_ptr<Context> c;{std::lock_guard<std::mutex> lock(registry_mutex);auto it=registry.find(reinterpret_cast<uintptr_t>(handle));if(it==registry.end())return;c=std::move(it->second);registry.erase(it);}c->engine.cancel();}
void orca_slicing_free_string(char* text){std::free(text);}
}
