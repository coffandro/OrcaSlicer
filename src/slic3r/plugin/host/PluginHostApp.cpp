#include "PluginHostBindings.hpp"
#include "PluginHostUi.hpp"
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r_version.h>
#include <slic3r/GUI/BuildCommit.hpp>
#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/OpenGLManager.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/Utils/CloudProvider.hpp>
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/Utils/bambu_networking.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/optional/optional.hpp>
#include <boost/system/error_code.hpp>

#include <exception>
#include <memory>
#include <regex>
#include <stdexcept>
#include <wx/app.h>
#include <wx/arrstr.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <string>
#include <utility>

namespace py = pybind11;

namespace Slic3r {
namespace {

GUI::Plater* current_plater()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");

    return plater;
}

PresetBundle* current_preset_bundle()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");

    return preset_bundle;
}

GUI::GUI_App& current_app()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return GUI::wxGetApp();
}

// The plater and the device list are only safe to touch from the UI thread, and the 3MF export
// renders thumbnails, which needs the UI thread's GL context.
void require_main_thread(const char* function_name)
{
    if (!wxIsMainThread())
        throw std::runtime_error(std::string(function_name) + "() must be called from the UI thread");
}

void export_3mf_copy(GUI::Plater& plater, const std::string& path)
{
    require_main_thread("export_3mf_copy");
    if (!boost::iends_with(path, ".3mf"))
        throw py::value_error("export_3mf_copy() needs a path ending in .3mf");
    // The thumbnails are rendered by the 3D view, which may not exist yet while plugins load.
    if (!GUI::OpenGLManager::get_gl_info().is_detected())
        throw std::runtime_error("export_3mf_copy() needs the 3D view, which is not ready yet");
    // The file is written from C++, so raise the audit event Python's open(path, "w") raises: the
    // plugin gets the same deny list, permissions and prompt as for writing the file itself.
    if (PySys_Audit("open", "ssi", path.c_str(), "w", 0) < 0)
        throw py::error_already_set();
    if (!plater.export_3mf_copy(GUI::into_path(GUI::from_u8(path))))
        throw std::runtime_error("Failed to write " + path);
}

// What PlaterDropTarget::OnDropFiles does around Plater::load_files: show the Prepare tab's 3D view so
// the load happens where the user can see it, then retitle the window. The window is not raised, so a
// background plugin doesn't steal focus. UI thread only.
static bool load_files_as_drop(GUI::Plater& plater, const wxArrayString& files)
{
    GUI::GUI_App& app = GUI::wxGetApp();
    if (app.mainframe != nullptr)
        app.mainframe->select_tab(TAB_ID_PREPARE);
    if (app.is_editor())
        plater.select_view_3D("3D");
    const bool loaded = plater.load_files(files);
    if (app.mainframe != nullptr)
        app.mainframe->update_title();
    return loaded;
}

// Add files to the plate as dropping them on the 3D view does. Callable from any thread except a
// SlicingPipeline hook: the load stops background processing, and the UI thread can be parked waiting on
// the slicing worker, so a call from a hook can deadlock (see PluginHostUi.hpp). On the UI thread the load
// is queued rather than run inline: a page or window callback runs inside the web view's script handler,
// and the load's dialogs must not nest on that native stack (see PluginHostUi's create_window).
void load_files(GUI::Plater& plater, const py::iterable& paths)
{
    // The extensions Plater::load_files(const wxArrayString&) accepts; it skips any other file silently.
    static const std::regex supported(".*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip|drc|glb|gltf|fbx|gcode|g)",
                                      std::regex::icase);
    // A str is iterable too, one character at a time, so reject it rather than report each character.
    if (py::isinstance<py::str>(paths) || py::isinstance<py::bytes>(paths))
        throw py::type_error("load_files() takes a list of paths, not a single path");
    const py::object fspath = py::module_::import("os").attr("fspath");
    wxArrayString files;
    for (const py::handle item : paths) {
        const std::string path = fspath(item).cast<std::string>();
        const boost::filesystem::path fs_path = GUI::into_path(GUI::from_u8(path));
        boost::system::error_code ec;
        if (!boost::filesystem::is_regular_file(fs_path, ec))
            throw py::value_error("Not a file: " + path);
        if (!std::regex_match(fs_path.filename().string(), supported))
            throw py::value_error("Not a file type the plate accepts: " + path);
        // The file is read from C++, so raise the audit event Python's open(path, "r") raises.
        if (PySys_Audit("open", "ssi", path.c_str(), "r", 0) < 0)
            throw py::error_already_set();
        files.Add(GUI::from_u8(path));
    }
    if (files.empty())
        throw py::value_error("load_files() needs at least one path");
    if (wxIsMainThread()) {
        GUI::wxGetApp().CallAfter([files] {
            // Same rules as create_window's deferral: the app may be closing by now, and an exception
            // escaping into the main loop ends it (GUI_App::OnExceptionInMainLoop returns false).
            if (GUI::wxGetApp().is_closing())
                return;
            try {
                if (GUI::Plater* plater = GUI::wxGetApp().plater())
                    load_files_as_drop(*plater, files);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Plugin load_files failed: " << e.what();
            }
        });
        return;
    }
    bool loaded = false;
    PluginHostUi::run_on_ui_thread([&] { loaded = load_files_as_drop(plater, files); });
    if (!loaded)
        throw std::runtime_error("The files were not loaded (the load failed or was cancelled)");
}

py::object gl_info()
{
    const GUI::OpenGLManager::GLInfo& info = GUI::OpenGLManager::get_gl_info();
    if (!info.is_detected())
        return py::none();
    py::dict out;
    out["vendor"]       = info.get_vendor();
    out["renderer"]     = info.get_renderer();
    out["version"]      = info.get_version();
    out["glsl_version"] = info.get_glsl_version();
    out["core_profile"] = info.is_core_profile();
    return std::move(out);
}

py::dict app_info()
{
    GUI::GUI_App& app = current_app();
    // The app config has no lock of its own; the UI thread is where it is written.
    require_main_thread("app_info");
    py::dict out;
    out["version"]  = SoftFever_VERSION;
    out["build"]    = GUI::build_commit_label;
    out["mode"]     = app.is_editor() ? "editor" : "gcode viewer";
    out["language"] = GUI::into_u8(app.current_language_code_safe());
    // The app config file is deny-listed for plugins; these are the parts a bug report needs.
    const boost::optional<Semver>& config_version = app.last_config_version();
    out["app_config_version"] = config_version && config_version->valid() ? config_version->to_string_sf() : std::string();
    out["stealth_mode"]       = app.app_config != nullptr && app.app_config->get_stealth_mode();
    out["is_signed_in"]       = app.is_user_login(ORCA_CLOUD_PROVIDER);
    out["is_bambu_signed_in"] = app.is_user_login(BBL_CLOUD_PROVIDER);
    out["network_plugin_version"] = NetworkAgent::is_network_module_loaded() ? NetworkAgent::get_version() : std::string();
    const NetworkLibraryLoadError load_error = NetworkAgent::get_load_error();
    out["network_plugin_error"] = load_error.has_error ? load_error.message : std::string();
    return out;
}

py::object selected_printer()
{
    GUI::GUI_App& app = current_app();
    require_main_thread("selected_printer");
    DeviceManager* devices = app.getDeviceManager();
    MachineObject* machine = devices != nullptr ? devices->get_selected_machine() : nullptr;
    if (machine == nullptr)
        return py::none();
    // Agent, model, link and firmware only: no name, serial, address or access code. Other agents
    // fill the model and firmware with placeholders, so those come from Bambu printers only.
    const bool bambu = machine->printer_agent_id == BBL_PRINTER_AGENT_ID;
    py::dict   out;
    out["agent"]      = machine->printer_agent_id;
    out["model_id"]   = bambu ? machine->printer_type : std::string();
    out["connection"] = machine->connection_type();
    out["online"]     = machine->is_online();
    out["connected"]  = machine->is_connected();
    out["firmware"]   = bambu ? machine->get_ota_version() : std::string();
    return std::move(out);
}

} // namespace

// Access to the live GUI application: the Plater and the module-level
// plater()/model()/preset_bundle() accessors. Everything here is owned by the
// app and only reachable once the GUI is up (the accessors throw before that).
void host_bindings::register_app(py::module_& host)
{
    py::class_<GUI::Plater, std::unique_ptr<GUI::Plater, py::nodelete>>(host, "Plater",
        "The app's project editor: the live project's Model and its saved state. Get it with orca.host.plater().")
        .def("model", static_cast<Model& (GUI::Plater::*)()>(&GUI::Plater::model),
             py::return_value_policy::reference_internal,
             "The live project's Model, the same one orca.host.model() returns.")
        .def("is_project_dirty", &GUI::Plater::is_project_dirty,
             "True when the project has unsaved changes: to its objects and plates, its project settings or its "
             "presets (see is_presets_dirty()).")
        .def("is_presets_dirty", &GUI::Plater::is_presets_dirty,
             "True when any preset has unsaved edits or, for a project opened from or saved to a file, when the "
             "printer, process, filament presets or filament colours differ from the ones it was opened or saved with.")
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture,
             "True while the app is inside an edit grouped into a single undo step, during which it takes no further "
             "undo snapshots.")
        .def("project_path", [](GUI::Plater& plater) {
            require_main_thread("project_path");
            return GUI::into_u8(plater.get_project_filename(".3mf"));
        }, "Path of the project's .3mf file, where it was last opened from or saved to; empty while it has never been "
           "saved. UI thread only: from a worker thread, where script_run scripts run, it raises RuntimeError.")
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Write the current project, unsaved changes included, to path as a 3MF copy. The project's "
             "file name and saved state are not changed, and the signed-in account is not written as the designer. "
             "The path must end in .3mf (ValueError otherwise), and writing it goes through the same permission check "
             "as open(path, \"w\"). Raises RuntimeError while the 3D view is not ready yet. UI thread only: from a "
             "worker thread, where script_run scripts run, it raises RuntimeError.")
        .def("load_files", &load_files, py::arg("paths"),
             "Add files to the plate as dropping them on the 3D view does. paths is an iterable of str or "
             "os.PathLike. Models are added to the project, a project file asks how to open it, and a G-code file "
             "replaces the project and opens in the preview. The Prepare tab's 3D view is shown first. A path that "
             "isn't an existing file of a type the plate accepts raises ValueError before anything is loaded. "
             "From a background thread this waits for the load and raises RuntimeError when the files were not "
             "loaded, which includes the user cancelling one of the load's prompts; from the UI thread the load is "
             "queued and this returns at once. Do not call it from a SlicingPipeline hook: the load waits for the "
             "UI thread, which can be waiting for the slicing worker, so the call can deadlock.");

    host.def("plater", &current_plater, py::return_value_policy::reference,
             "The app's Plater, which holds the live project. Raises RuntimeError before the app's main window is up.");
    host.def("model", []() -> Model& {
        return current_plater()->model();
    }, py::return_value_policy::reference,
       "The live project's Model: what the 3D editor shows, owned by the app and changed as the person or tools edit "
       "it. The objects, volumes and instances it hands out are references into it, so fetch them again after the "
       "project changes rather than keeping them. Raises RuntimeError before the app's main window is up.");
    host.def("preset_bundle", &current_preset_bundle, py::return_value_policy::reference,
             "The app's live PresetBundle: every loaded printer, process and filament preset and the ones selected for "
             "the project. Raises RuntimeError while the app is still loading its presets at startup.");
    host.def("app_info", &app_info,
             "The running app: version, build, mode, language, app_config_version (the version that wrote the app config "
             "before this run, empty when there was none), stealth_mode, is_signed_in (Orca Cloud), is_bambu_signed_in (Bambu Cloud), network_plugin_version (empty "
             "when the Bambu network plugin is not loaded) and network_plugin_error (empty unless it failed to load). "
             "UI thread only: from a worker thread, where script_run scripts run, it raises RuntimeError.");
    host.def("gl_info", &gl_info,
             "OpenGL vendor, renderer, version, glsl_version and core_profile of the 3D view; None before it has been created.");
    host.def("selected_printer", &selected_printer,
             "Agent, model_id, connection ('lan' or 'cloud'), online, connected and firmware of the printer selected in the "
             "device list; model_id and firmware are empty for non-Bambu agents. None when none is selected. Print hosts "
             "configured in the printer preset (OctoPrint and the like) are not in the device list. UI thread only: from "
             "a worker thread, where script_run scripts run, it raises RuntimeError.");
}

} // namespace Slic3r
