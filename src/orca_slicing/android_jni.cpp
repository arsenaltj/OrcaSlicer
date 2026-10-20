#ifdef __ANDROID__
#include <jni.h>
#include "include/orca/slicing_c.h"
#include <string>
namespace {
std::string utf8(JNIEnv* e,jstring s){if(!s)return {};const char* p=e->GetStringUTFChars(s,nullptr);if(!p)return {};std::string result(p);e->ReleaseStringUTFChars(s,p);return result;}
struct Callback{JavaVM* vm; jobject receiver;jmethodID method;orca_slicing_context* handle;};
void progress(const char* json,void* data){auto& c=*static_cast<Callback*>(data);JNIEnv* e=nullptr;bool attached=false;if(c.vm->GetEnv(reinterpret_cast<void**>(&e),JNI_VERSION_1_6)!=JNI_OK){if(c.vm->AttachCurrentThread(&e,nullptr)!=JNI_OK)return;attached=true;}jstring s=e->NewStringUTF(json?json:"{}");if(s){e->CallVoidMethod(c.receiver,c.method,s);e->DeleteLocalRef(s);}if(e->ExceptionCheck()){e->ExceptionClear();orca_slicing_cancel(c.handle);}if(attached)c.vm->DetachCurrentThread();}
}
extern "C" JNIEXPORT jlong JNICALL Java_com_orca_mobile_slicing_NativeSlicing_nativeCreate(JNIEnv* e,jclass,jstring root){auto text=utf8(e,root);return reinterpret_cast<jlong>(orca_slicing_create(text.c_str()));}
extern "C" JNIEXPORT jstring JNICALL Java_com_orca_mobile_slicing_NativeSlicing_nativeCapabilities(JNIEnv* e,jclass){return e->NewStringUTF(orca_slicing_capabilities());}
extern "C" JNIEXPORT void JNICALL Java_com_orca_mobile_slicing_NativeSlicing_nativeCancel(JNIEnv*,jclass,jlong h){orca_slicing_cancel(reinterpret_cast<orca_slicing_context*>(h));}
extern "C" JNIEXPORT void JNICALL Java_com_orca_mobile_slicing_NativeSlicing_nativeDestroy(JNIEnv*,jclass,jlong h){orca_slicing_destroy(reinterpret_cast<orca_slicing_context*>(h));}
extern "C" JNIEXPORT jstring JNICALL Java_com_orca_mobile_slicing_NativeSlicing_nativeExecute(JNIEnv* e,jclass,jlong handle,jstring request,jobject callback){
 auto text=utf8(e,request);auto* context=reinterpret_cast<orca_slicing_context*>(handle);char* result=nullptr;Callback c{};e->GetJavaVM(&c.vm);c.handle=context;if(callback){c.receiver=e->NewGlobalRef(callback);jclass cls=e->GetObjectClass(callback);c.method=e->GetMethodID(cls,"onNativeEvent","(Ljava/lang/String;)V");e->DeleteLocalRef(cls);if(!c.method){e->ExceptionClear();e->DeleteGlobalRef(c.receiver);return e->NewStringUTF("{\"ok\":false,\"error\":{\"code\":\"INVALID_CALLBACK\"}}");}}
 orca_slicing_execute(context,text.c_str(),c.receiver?progress:nullptr,c.receiver?&c:nullptr,&result);jstring answer=e->NewStringUTF(result?result:"{\"ok\":false,\"error\":{\"code\":\"NATIVE_RESULT_MISSING\"}}");orca_slicing_free_string(result);if(c.receiver)e->DeleteGlobalRef(c.receiver);return answer;
}
#endif
