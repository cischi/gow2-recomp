// Shim of Xenia's src/xenia/base/cvar.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec and
// xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad,
// BSD-3-Clause): same DEFINE_*/DECLARE_* macros (bool, int32, uint32,
// uint64, double, string, path), with a registry by name in place of Xenia's
// TOML configuration file and command line.
//
// DEFINE_x(name, ...) creates the variable cvars::name and registers it when
// the module loads; gow2::xenia::SetCvar changes it by name.
#ifndef XENIA_CVAR_H_
#define XENIA_CVAR_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cvar {

class IConfigVar {
 public:
  virtual ~IConfigVar() = default;
  virtual const std::string& name() const = 0;
  // false if the text is not a valid value for the type (variable untouched).
  virtual bool SetFromString(std::string_view value) = 0;
};

// Text -> value conversion, false if invalid (defined in cvar.cpp).
bool ParseValue(std::string_view text, bool& out);
bool ParseValue(std::string_view text, int32_t& out);
bool ParseValue(std::string_view text, uint32_t& out);
bool ParseValue(std::string_view text, uint64_t& out);
bool ParseValue(std::string_view text, double& out);
bool ParseValue(std::string_view text, std::string& out);
bool ParseValue(std::string_view text, std::filesystem::path& out);

template <class T>
class ConfigVar final : public IConfigVar {
 public:
  ConfigVar(const char* name, T* value) : name_(name), value_(value) {}
  const std::string& name() const override { return name_; }
  bool SetFromString(std::string_view text) override {
    T parsed{};
    if (!ParseValue(text, parsed)) {
      return false;
    }
    *value_ = std::move(parsed);
    return true;
  }

 private:
  std::string name_;
  T* value_;
};

// Adds the variable to the registry (which takes ownership).
void Register(IConfigVar* var);

template <class T>
IConfigVar* define_configvar(const char* name, T* value, const char* /*description*/,
                             const char* /*category*/, bool /*is_transient*/) {
  IConfigVar* var = new ConfigVar<T>(name, value);
  Register(var);
  return var;
}

}  // namespace cvar

namespace gow2::xenia {
// Sets Xenia's cvar `name` from the text `value`: false if the cvar does not
// exist (undefined or not linked) or the value is not valid.
bool SetCvar(std::string_view name, std::string_view value);
// Names of the registered cvars, sorted.
std::vector<std::string> CvarNames();
}  // namespace gow2::xenia

#define DEFINE_bool(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, bool)

#define DEFINE_int32(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, int32_t)

#define DEFINE_uint32(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, uint32_t)

#define DEFINE_uint64(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, uint64_t)

#define DEFINE_double(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, double)

#define DEFINE_string(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, std::string)

#define DEFINE_path(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, false, std::filesystem::path)

#define DEFINE_transient_bool(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, true, bool)

#define DEFINE_transient_string(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, true, std::string)

#define DEFINE_transient_path(name, default_value, description, category) \
  DEFINE_CVar(name, default_value, description, category, true, std::filesystem::path)

#define DEFINE_CVar(name, default_value, description, category, is_transient, type) \
  namespace cvars {                                                                  \
  type name = default_value;                                                         \
  }                                                                                  \
  namespace cv {                                                                     \
  static cvar::IConfigVar* const cv_##name =                                        \
      cvar::define_configvar(#name, &cvars::name, description, category, is_transient); \
  }

#define DECLARE_bool(name) DECLARE_CVar(name, bool)

#define DECLARE_int32(name) DECLARE_CVar(name, int32_t)

#define DECLARE_uint32(name) DECLARE_CVar(name, uint32_t)

#define DECLARE_uint64(name) DECLARE_CVar(name, uint64_t)

#define DECLARE_double(name) DECLARE_CVar(name, double)

#define DECLARE_string(name) DECLARE_CVar(name, std::string)

#define DECLARE_path(name) DECLARE_CVar(name, std::filesystem::path)

#define DECLARE_CVar(name, type) \
  namespace cvars {              \
  extern type name;              \
  }

// xenia-canary: changes the cvar's value from code (GPU settings saved by
// Xenia); here a plain assignment.
#define OVERRIDE_CVar(name, type, value) (cvars::name = static_cast<type>(value))
#define OVERRIDE_bool(name, value) OVERRIDE_CVar(name, bool, value)
#define OVERRIDE_int32(name, value) OVERRIDE_CVar(name, int32_t, value)
#define OVERRIDE_uint32(name, value) OVERRIDE_CVar(name, uint32_t, value)
#define OVERRIDE_uint64(name, value) OVERRIDE_CVar(name, uint64_t, value)
#define OVERRIDE_double(name, value) OVERRIDE_CVar(name, double, value)
#define OVERRIDE_string(name, value) OVERRIDE_CVar(name, std::string, value)
#define OVERRIDE_path(name, value) OVERRIDE_CVar(name, std::filesystem::path, value)

// xenia-canary: update of the default saved in the user's configuration file.
// gow2 has no such file: no effect (the trailing ; stays valid).
#define UPDATE_from_any(...) static_assert(true)
#define UPDATE_CVar(...) static_assert(true)
#define UPDATE_from_bool(...) static_assert(true)
#define UPDATE_from_int32(...) static_assert(true)
#define UPDATE_from_uint32(...) static_assert(true)
#define UPDATE_from_uint64(...) static_assert(true)
#define UPDATE_from_double(...) static_assert(true)
#define UPDATE_from_string(...) static_assert(true)
#define UPDATE_from_path(...) static_assert(true)

#endif  // XENIA_CVAR_H_
