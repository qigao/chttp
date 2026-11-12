#ifndef CHTTP_APPLICATION_HPP
#define CHTTP_APPLICATION_HPP
#include <chttp_app/application.h>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>

namespace chttp {
/** Noncopyable RAII owner. Call close() explicitly to handle shutdown errors;
 * an unhandled cleanup error in the noexcept destructor terminates. */
class Application final {
  chttp_application owner_{};
public:
  Application() = default;
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;
  ~Application() noexcept {
    if (owner_.impl != nullptr && close() != SALTS_OK) std::terminate();
  }
  int initialize(const chttp_application_config& config) { return chttp_application_init(&owner_, &config); }
  int start() { return chttp_application_start(&owner_); }
  int port(uint16_t& value) const { return chttp_application_port(&owner_, &value); }
  int close(uint32_t timeout_ms = 0u) { return chttp_application_close(&owner_, timeout_ms); }
  bool valid() const noexcept { return owner_.impl != nullptr; }
};

/** Canonical ACE Provider with native C++ object ownership. Factory, DataDesc,
 * Component and Interface metadata must outlive Application. No hidden registry.
 * Factory returns unique_ptr<T> and receives the canonical dependency array.
 * Exceptions are translated before returning through ACE's C callbacks. */
template<class T, class Factory> class Bean final {
  static_assert(std::is_nothrow_destructible_v<T>);
  Factory& factory_;
  const cmeta_data_desc *data_;
  static void destroy(void*, void *object) noexcept { delete static_cast<T*>(object); }
  static inline const cmeta_object_lifecycle lifecycle_{sizeof(cmeta_object_lifecycle), nullptr,
      nullptr, nullptr, destroy};
  static cmeta_status create(void *context, const cmeta_data_desc *config_data,
      const void *config, const salts_configurator_dependency *deps, size_t count,
      cmeta_object_ref *out) noexcept {
    auto& bean = *static_cast<Bean*>(context);
    try {
      auto object = bean.factory_(config_data, config, deps, count);
      if (!object) return CMETA_OUT_OF_MEMORY;
      cmeta_status status = cmeta_object_borrow(out, object.get(), bean.data_, nullptr);
      if (status != CMETA_OK) return status;
      status = cmeta_object_take(out, &lifecycle_);
      if (status == CMETA_OK) object.release();
      else (void)cmeta_object_release(out);
      return status;
    } catch (const std::bad_alloc&) { return CMETA_OUT_OF_MEMORY; }
    catch (...) { return CMETA_CALLBACK_ERROR; }
  }
  salts_configurator_provider_binding binding_;
public:
  Bean(const cmeta_component_desc *component, const cmeta_data_desc *data,
      Factory& factory, const cmeta_object_interface_provider *interfaces = nullptr)
      : factory_(factory), data_(data), binding_{sizeof(binding_),
          SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, component, this, interfaces,
          create, nullptr, nullptr} {}
  Bean(const Bean&) = delete;
  Bean& operator=(const Bean&) = delete;
  salts_configurator_deployment deployment(const cmeta_data_desc *data = nullptr,
      const void *value = nullptr) const noexcept { return {&binding_, data, value}; }
};

template<chttp_action_fn Action> chttp_action mapping(chttp_method method,
    const char *path, const cmeta_function_abi_desc *abi) noexcept {
  const auto invoke = [](void *self, const chttp_exchange *exchange) noexcept -> int {
    try { return Action(self, exchange); }
    catch (...) { return SALTS_EIO; }
  };
  return {method, path, abi, invoke, nullptr, nullptr};
}
}
#define CHTTP_CPP_GET(path_, action_) \
  ::chttp::mapping<action_>(CHTTP_METHOD_GET, path_, FunctionAbi(action_))
#endif
