#include "chromecast/android_platform.h"

#include <pthread.h>

#include <cstdarg>
#include <new>
#include <utility>

namespace chromecast {
struct Platform {
  JavaVM* vm = nullptr;
  jobject context = nullptr;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  int next = 1;
  // Pending intents of active requests, as global references.
  std::vector<std::pair<int, jobject>> requests;
};

namespace {
constexpr int kTransportWifi = 1, kCapabilityInternet = 12, kPatternPrefix = 1;
constexpr int kFlagImmutable = 0x04000000;

// The calling thread's JNI environment, attached for the duration of a call
// when the thread does not belong to the Java VM.
struct Attachment {
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  bool attached = false;
};

void destroy(Attachment* a) noexcept {
  if (a->attached) a->vm->DetachCurrentThread();
  delete a;
}

common::Owner<Attachment> attach(JavaVM* vm) {
  common::Owner<Attachment> a(new (std::nothrow) Attachment{vm});
  if (!a) return a;
  void* env = nullptr;
  jint state = vm->GetEnv(&env, JNI_VERSION_1_6);
  if (state == JNI_EDETACHED) {
    if (vm->AttachCurrentThread(&a->env, nullptr) != JNI_OK) return {};
    a->attached = true;
  } else if (state != JNI_OK) {
    return {};
  } else {
    a->env = static_cast<JNIEnv*>(env);
  }
  return a;
}

// Clears a pending exception, returning its description.
std::string take_exception(JNIEnv* env) {
  jthrowable thrown = env->ExceptionOccurred();
  if (!thrown) return {};
  env->ExceptionClear();
  std::string text = "Android error";
  jclass type = env->GetObjectClass(thrown);
  jmethodID describe = env->GetMethodID(type, "toString", "()Ljava/lang/String;");
  auto value = describe ? static_cast<jstring>(env->CallObjectMethod(thrown, describe)) : nullptr;
  if (env->ExceptionCheck()) env->ExceptionClear();
  else if (value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars) {
      text = chars;
      env->ReleaseStringUTFChars(value, chars);
    }
  }
  return text;
}

// JNI calls that turn exceptions and missing methods into null results.
struct Calls {
  JNIEnv* env;
  std::string error;

  bool check() {
    if (!env->ExceptionCheck()) return true;
    std::string text = take_exception(env);
    if (error.empty()) error = text;
    return false;
  }

  jmethodID method(jobject object, const char* name, const char* signature) {
    if (!object) return nullptr;
    jclass type = env->GetObjectClass(object);
    jmethodID id = env->GetMethodID(type, name, signature);
    env->DeleteLocalRef(type);
    return check() ? id : nullptr;
  }

  jobject object(jobject target, const char* name, const char* signature, ...) {
    jmethodID id = method(target, name, signature);
    if (!id) return nullptr;
    va_list arguments;
    va_start(arguments, signature);
    jobject result = env->CallObjectMethodV(target, id, arguments);
    va_end(arguments);
    return check() ? result : nullptr;
  }

  bool boolean(jobject target, const char* name, const char* signature, ...) {
    jmethodID id = method(target, name, signature);
    if (!id) return false;
    va_list arguments;
    va_start(arguments, signature);
    jboolean result = env->CallBooleanMethodV(target, id, arguments);
    va_end(arguments);
    return check() && result;
  }

  jint integer(jobject target, const char* name, const char* signature, ...) {
    jmethodID id = method(target, name, signature);
    if (!id) return -1;
    va_list arguments;
    va_start(arguments, signature);
    jint result = env->CallIntMethodV(target, id, arguments);
    va_end(arguments);
    return check() ? result : -1;
  }

  jlong wide(jobject target, const char* name, const char* signature) {
    jmethodID id = method(target, name, signature);
    if (!id) return 0;
    jlong result = env->CallLongMethod(target, id);
    return check() ? result : 0;
  }

  bool call(jobject target, const char* name, const char* signature, ...) {
    jmethodID id = method(target, name, signature);
    if (!id) return false;
    va_list arguments;
    va_start(arguments, signature);
    env->CallVoidMethodV(target, id, arguments);
    va_end(arguments);
    return check();
  }

  jclass type(const char* name) {
    jclass found = env->FindClass(name);
    return check() ? found : nullptr;
  }

  jobject create(const char* name, const char* signature, ...) {
    jclass found = type(name);
    if (!found) return nullptr;
    jmethodID id = env->GetMethodID(found, "<init>", signature);
    if (!check() || !id) return nullptr;
    va_list arguments;
    va_start(arguments, signature);
    jobject result = env->NewObjectV(found, id, arguments);
    va_end(arguments);
    return check() ? result : nullptr;
  }

  jobject statics(const char* name, const char* method_name, const char* signature, ...) {
    jclass found = type(name);
    if (!found) return nullptr;
    jmethodID id = env->GetStaticMethodID(found, method_name, signature);
    if (!check() || !id) return nullptr;
    va_list arguments;
    va_start(arguments, signature);
    jobject result = env->CallStaticObjectMethodV(found, id, arguments);
    va_end(arguments);
    return check() ? result : nullptr;
  }

  jstring string(const std::string& value) {
    jstring result = env->NewStringUTF(value.c_str());
    return check() ? result : nullptr;
  }

  jobject connectivity(jobject context) {
    jstring name = string("connectivity");
    return name
               ? object(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", name)
               : nullptr;
  }
};

struct Frame {
  JNIEnv* env;
  bool pushed;
};

void destroy(Frame* f) noexcept {
  if (f->pushed) f->env->PopLocalFrame(nullptr);
  delete f;
}

common::Owner<Frame> push_frame(JNIEnv* env, int capacity) {
  bool pushed = env->PushLocalFrame(capacity) == 0;
  if (!pushed) env->ExceptionClear();
  common::Owner<Frame> frame(new (std::nothrow) Frame{env, pushed});
  if (frame && !pushed) frame.reset();
  else if (!frame && pushed) env->PopLocalFrame(nullptr);
  return frame;
}

auto failure(std::string message) { return std::unexpected(common::Error{std::move(message)}); }
}

common::Result<common::Owner<Platform>> create_platform(JavaVM* vm, JNIEnv* env, jobject context) {
  common::Owner<Platform> p(new (std::nothrow) Platform);
  if (!p) return failure("Cannot allocate the Wi-Fi access state");
  auto frame = push_frame(env, 8);
  if (!frame) return failure("Cannot use Android's Java runtime");
  Calls c{env};
  jobject application = c.object(context, "getApplicationContext", "()Landroid/content/Context;");
  if (!application) return failure("Cannot read the application context");
  p->vm = vm;
  p->context = env->NewGlobalRef(application);
  if (!p->context) return failure("Cannot keep the application context");
  return p;
}

void destroy(Platform* p) noexcept {
  while (!p->requests.empty()) release_network(*p, p->requests.back().first);
  if (p->context)
    if (auto a = attach(p->vm)) a->env->DeleteGlobalRef(p->context);
  pthread_mutex_destroy(&p->mutex);
  delete p;
}

std::vector<PhoneNetwork> list_networks(Platform& p) {
  std::vector<PhoneNetwork> result;
  auto a = attach(p.vm);
  if (!a) return result;
  JNIEnv* env = a->env;
  auto frame = push_frame(env, 16);
  if (!frame) return result;
  Calls c{env};
  jobject manager = c.connectivity(p.context);
  auto all =
      static_cast<jobjectArray>(c.object(manager, "getAllNetworks", "()[Landroid/net/Network;"));
  jsize count = all ? env->GetArrayLength(all) : 0;
  for (jsize i = 0; i < count; ++i) {
    auto inner = push_frame(env, 32);
    if (!inner) break;
    jobject network = env->GetObjectArrayElement(all, i);
    jobject capabilities =
        c.object(manager, "getNetworkCapabilities",
                 "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;", network);
    if (!c.boolean(capabilities, "hasTransport", "(I)Z", kTransportWifi)) continue;
    jlong handle = c.wide(network, "getNetworkHandle", "()J");
    jobject link = c.object(manager, "getLinkProperties",
                            "(Landroid/net/Network;)Landroid/net/LinkProperties;", network);
    jobject addresses = c.object(link, "getLinkAddresses", "()Ljava/util/List;");
    jint size = addresses ? c.integer(addresses, "size", "()I") : 0;
    for (jint j = 0; j < size; ++j) {
      jobject entry = c.object(addresses, "get", "(I)Ljava/lang/Object;", j);
      jobject address = c.object(entry, "getAddress", "()Ljava/net/InetAddress;");
      auto bytes = static_cast<jbyteArray>(c.object(address, "getAddress", "()[B"));
      if (!bytes || env->GetArrayLength(bytes) != 4) continue;
      jbyte raw[4];
      env->GetByteArrayRegion(bytes, 0, 4, raw);
      PhoneNetwork n;
      n.handle = std::uint64_t(handle);
      n.address = std::uint32_t(std::uint8_t(raw[0])) << 24 |
                  std::uint32_t(std::uint8_t(raw[1])) << 16 |
                  std::uint32_t(std::uint8_t(raw[2])) << 8 | std::uint8_t(raw[3]);
      n.prefix = c.integer(entry, "getPrefixLength", "()I");
      if (n.handle && n.address && n.prefix > 0) result.push_back(n);
      break;
    }
  }
  return result;
}

common::Result<int> request_network(Platform& p, const WifiRequest& r) {
  auto a = attach(p.vm);
  if (!a) return failure("Cannot use Android's Java runtime");
  JNIEnv* env = a->env;
  auto frame = push_frame(env, 32);
  if (!frame) return failure("Cannot use Android's Java runtime");
  Calls c{env};
  constexpr char kSpecifier[] = "android/net/wifi/WifiNetworkSpecifier$Builder";
  constexpr char kReturns[] = "Landroid/net/wifi/WifiNetworkSpecifier$Builder;";
  std::string with_string = std::string("(Ljava/lang/String;)") + kReturns;
  jobject builder = c.create(kSpecifier, "()V");
  if (!r.bssid.empty()) {
    jobject mac = c.statics("android/net/MacAddress", "fromString",
                            "(Ljava/lang/String;)Landroid/net/MacAddress;", c.string(r.bssid));
    builder = mac ? c.object(builder, "setBssid",
                             (std::string("(Landroid/net/MacAddress;)") + kReturns).c_str(), mac)
                  : nullptr;
  }
  if (builder && !r.ssid.empty()) {
    if (r.prefix) {
      jobject pattern = c.create("android/os/PatternMatcher", "(Ljava/lang/String;I)V",
                                 c.string(r.ssid), kPatternPrefix);
      builder =
          c.object(builder, "setSsidPattern",
                   (std::string("(Landroid/os/PatternMatcher;)") + kReturns).c_str(), pattern);
    } else {
      builder = c.object(builder, "setSsid", with_string.c_str(), c.string(r.ssid));
    }
  }
  if (builder && !r.passphrase.empty())
    builder = c.object(builder, r.wpa3 ? "setWpa3Passphrase" : "setWpa2Passphrase",
                       with_string.c_str(), c.string(r.passphrase));
  jobject specifier = c.object(builder, "build", "()Landroid/net/wifi/WifiNetworkSpecifier;");
  jobject request = c.create("android/net/NetworkRequest$Builder", "()V");
  constexpr char kRequestBuilder[] = "Landroid/net/NetworkRequest$Builder;";
  request = c.object(request, "addTransportType", (std::string("(I)") + kRequestBuilder).c_str(),
                     kTransportWifi);
  // Wi-Fi refuses specifier requests for internet access.
  request = c.object(request, "removeCapability", (std::string("(I)") + kRequestBuilder).c_str(),
                     kCapabilityInternet);
  request =
      specifier
          ? c.object(request, "setNetworkSpecifier",
                     (std::string("(Landroid/net/NetworkSpecifier;)") + kRequestBuilder).c_str(),
                     specifier)
          : nullptr;
  request = c.object(request, "build", "()Landroid/net/NetworkRequest;");
  // Results arrive as a broadcast nobody receives; the network itself shows up
  // in list_networks(), so no Java callback class is needed.
  jstring package =
      static_cast<jstring>(c.object(p.context, "getPackageName", "()Ljava/lang/String;"));
  jobject intent = c.create("android/content/Intent", "(Ljava/lang/String;)V",
                            c.string("dev.demo.chromecast.NETWORK"));
  intent = package ? c.object(intent, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;",
                              package)
                   : nullptr;
  pthread_mutex_lock(&p.mutex);
  int id = p.next++;
  pthread_mutex_unlock(&p.mutex);
  jobject pending = intent ? c.statics("android/app/PendingIntent", "getBroadcast",
                                       "(Landroid/content/Context;ILandroid/content/Intent;I)"
                                       "Landroid/app/PendingIntent;",
                                       p.context, id, intent, kFlagImmutable)
                           : nullptr;
  jobject manager = c.connectivity(p.context);
  if (!request || !pending || !manager ||
      !c.call(manager, "requestNetwork",
              "(Landroid/net/NetworkRequest;Landroid/app/PendingIntent;)V", request, pending))
    return failure(c.error.empty() ? "Android refused the Wi-Fi request" : c.error);
  jobject kept = env->NewGlobalRef(pending);
  if (!kept) return failure("Cannot keep the Wi-Fi request");
  pthread_mutex_lock(&p.mutex);
  p.requests.emplace_back(id, kept);
  pthread_mutex_unlock(&p.mutex);
  return id;
}

void release_network(Platform& p, int id) {
  pthread_mutex_lock(&p.mutex);
  jobject pending = nullptr;
  for (auto it = p.requests.begin(); it != p.requests.end(); ++it)
    if (it->first == id) {
      pending = it->second;
      p.requests.erase(it);
      break;
    }
  pthread_mutex_unlock(&p.mutex);
  if (!pending) return;
  auto a = attach(p.vm);
  if (!a) return;
  JNIEnv* env = a->env;
  if (auto frame = push_frame(env, 8)) {
    Calls c{env};
    jobject manager = c.connectivity(p.context);
    c.call(manager, "releaseNetworkRequest", "(Landroid/app/PendingIntent;)V", pending);
    c.call(pending, "cancel", "()V");
  }
  env->DeleteGlobalRef(pending);
}
}
