#include "endstone_exchange/umoney_gateway.hpp"

#include <endstone/endstone.hpp>

#include <pybind11/embed.h>

#include <cerrno>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace exchange {
namespace py = pybind11;

namespace {

[[nodiscard]] std::int64_t exactPythonInteger(const py::handle value, std::string_view method) {
    if (!PyLong_CheckExact(value.ptr())) {
        throw std::runtime_error(std::format("UMoney {} returned a value that is not an integer", method));
    }

    int overflow = 0;
    const auto converted = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
    if (overflow != 0) {
        throw std::overflow_error(std::format("UMoney {} returned an integer outside int64", method));
    }
    if (PyErr_Occurred() != nullptr) {
        throw py::error_already_set();
    }
    return converted;
}

void requireCallable(const py::object &plugin, const char *method) {
    if (!py::hasattr(plugin, method)) {
        throw std::runtime_error(std::format("UMoney plugin does not expose {}", method));
    }
    const auto attribute = plugin.attr(method);
    if (PyCallable_Check(attribute.ptr()) == 0) {
        throw std::runtime_error(std::format("UMoney attribute {} is not callable", method));
    }
}

#ifndef _WIN32
void syncPath(const std::filesystem::path &path, int flags, std::string_view label) {
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                std::format("open UMoney {} for durable sync", label));
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(),
                                std::format("sync UMoney {}", label));
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                std::format("close UMoney {} after durable sync", label));
    }
}

void syncUmoneyData(const std::filesystem::path &data_folder) {
    syncPath(data_folder / "money.json", O_RDONLY, "money.json");
    syncPath(data_folder, O_RDONLY | O_DIRECTORY, "data directory");
}
#else
void syncUmoneyData(const std::filesystem::path &) {}
#endif

} // namespace

struct UmoneyGateway::Impl {
    endstone::Server &server;
    EconomyConfig config;

    [[nodiscard]] endstone::Plugin &plugin() const {
        auto &manager = server.getPluginManager();
        auto *resolved = manager.getPlugin(config.umoney_plugin);
        if (resolved == nullptr) {
            throw std::runtime_error(std::format("UMoney plugin '{}' is not loaded", config.umoney_plugin));
        }
        if (!manager.isPluginEnabled(resolved)) {
            throw std::runtime_error(std::format("UMoney plugin '{}' is not enabled", config.umoney_plugin));
        }
        return *resolved;
    }

    [[nodiscard]] static py::object pythonObject(endstone::Plugin &plugin) {
        return py::cast(&plugin, py::return_value_policy::reference);
    }
};

UmoneyGateway::UmoneyGateway(endstone::Server &server, EconomyConfig config)
    : impl_(std::make_unique<Impl>(Impl{server, std::move(config)})) {}

UmoneyGateway::~UmoneyGateway() = default;

void UmoneyGateway::validate() const {
    py::gil_scoped_acquire gil;
    auto plugin = Impl::pythonObject(impl_->plugin());
    requireCallable(plugin, "api_get_player_money");
    requireCallable(plugin, "api_change_player_money");
}

std::optional<std::int64_t> UmoneyGateway::balance(std::string_view player_name) const {
    py::gil_scoped_acquire gil;
    auto plugin = Impl::pythonObject(impl_->plugin());
    requireCallable(plugin, "api_get_player_money");
    const auto result = plugin.attr("api_get_player_money")(std::string(player_name));
    if (result.is_none()) {
        return std::nullopt;
    }
    return exactPythonInteger(result, "api_get_player_money");
}

void UmoneyGateway::change(std::string_view player_name, std::int64_t delta_units) const {
    if (delta_units == 0) {
        throw std::invalid_argument("UMoney balance delta cannot be zero");
    }

    py::gil_scoped_acquire gil;
    auto &native_plugin = impl_->plugin();
    auto plugin = Impl::pythonObject(native_plugin);
    requireCallable(plugin, "api_change_player_money");
    plugin.attr("api_change_player_money")(std::string(player_name), delta_units);
    syncUmoneyData(native_plugin.getDataFolder());
}

} // namespace exchange
