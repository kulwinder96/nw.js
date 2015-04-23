#include "nw_content.h"
#include "nw_package.h"

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_restrictions.h"
#include "content/nw/src/common/shell_switches.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"

#include "extensions/renderer/script_context.h"
#include "extensions/common/extension.h"
#include "extensions/common/feature_switch.h"
#include "extensions/common/features/feature.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_constants.h"

#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebSecurityPolicy.h"

#include "chrome/common/chrome_version_info_values.h"

#include "ui/gfx/image/image.h"

// NEED TO STAY SYNC WITH NODE
#ifndef NODE_CONTEXT_EMBEDDER_DATA_INDEX
#define NODE_CONTEXT_EMBEDDER_DATA_INDEX 32
#endif

#include "third_party/node/src/node_webkit.h"
#include "third_party/WebKit/public/web/WebScopedMicrotaskSuppression.h"
#include "nw/id/commit.h"
#include "content/nw/src/nw_version.h"

using extensions::ScriptContext;
using extensions::Manifest;
using extensions::Feature;

namespace manifest_keys = extensions::manifest_keys;

namespace nw {

namespace {
v8::Handle<v8::Value> CallNWTickCallback(node::Environment* env, const v8::Handle<v8::Value> ret) {
  blink::WebScopedMicrotaskSuppression suppression;
  return node::CallTickCallback(env, ret);
}

v8::Handle<v8::Value> CreateNW(ScriptContext* context,
                               v8::Handle<v8::Object> node_global,
                               v8::Handle<v8::Context> node_context) {
  v8::Handle<v8::String> nw_string(
      v8::String::NewFromUtf8(context->isolate(), "nw"));
  v8::Handle<v8::Object> global(context->v8_context()->Global());
  v8::Handle<v8::Value> nw(global->Get(nw_string));
  if (nw->IsUndefined()) {
    nw = v8::Object::New(context->isolate());;
    //node_context->Enter();
    global->Set(nw_string, nw);
    //node_context->Exit();
  }
  return nw;
}

// Returns |value| cast to an object if possible, else an empty handle.
v8::Handle<v8::Object> AsObjectOrEmpty(v8::Handle<v8::Value> value) {
  return value->IsObject() ? value.As<v8::Object>() : v8::Handle<v8::Object>();
}

} //namespace

static Package* g_package;

int MainPartsPreCreateThreadsHook() {
  base::ThreadRestrictions::ScopedAllowIO allow_io;
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  command_line->AppendSwitch(switches::kNoSandbox);
  if (!g_package)
    g_package = new Package();
  if (g_package && !g_package->path().empty()) {
    base::FilePath path = g_package->path().NormalizePathSeparators();

    command_line->AppendArgPath(path);
  }
  return content::RESULT_CODE_NORMAL_EXIT;
}

void MainPartsPostDestroyThreadsHook() {
  if (g_package) {
    delete g_package;
    g_package = nullptr;
  }
}

Package* package() {
  if (!g_package)
    g_package = new Package();
  return g_package;
}

void DocumentElementHook(blink::WebFrame* frame,
                         const extensions::Extension* extension,
                         const GURL& effective_document_url) {
  frame->document().securityOrigin().grantUniversalAccess();
}

void ContextCreationHook(ScriptContext* context) {
  v8::Isolate* isolate = context->isolate();
  if (node::g_context.IsEmpty()) {
    node::SetupNWNode(0, nullptr);
    {
      int argc = 1;
      char argv0[] = "node";
      char* argv[2];
      argv[0] = argv0;
      argv[1] = nullptr;

      v8::Isolate* isolate = v8::Isolate::GetCurrent();
      v8::HandleScope scope(isolate);
      blink::WebScopedMicrotaskSuppression suppression;

      node::SetNWTickCallback(CallNWTickCallback);
      v8::Local<v8::Context> dom_context = context->v8_context();
      node::g_context.Reset(isolate, dom_context);
      dom_context->SetSecurityToken(v8::String::NewFromUtf8(isolate, "nw-token"));
      dom_context->Enter();
      dom_context->SetEmbedderData(0, v8::String::NewFromUtf8(isolate, "node"));

      node::StartNWInstance(argc, argv, dom_context);
      v8::Local<v8::Script> script = v8::Script::Compile(v8::String::NewFromUtf8(isolate,
                                                                                 "process.versions['nwjs'] = '" NW_VERSION_STRING "';"
                                                                                 "process.versions['node-webkit'] = '" NW_VERSION_STRING "';"
                                                                                 "process.versions['nw-commit-id'] = '" NW_COMMIT_HASH "';"
                                                                                 "process.versions['chromium'] = '" PRODUCT_VERSION "';"
                                                                                 ));
      script->Run();
      dom_context->Exit();
    }
  }
  v8::Local<v8::Context> g_context =
    v8::Local<v8::Context>::New(isolate, node::g_context);
  v8::Local<v8::Object> node_global = g_context->Global();

  context->v8_context()->SetAlignedPointerInEmbedderData(NODE_CONTEXT_EMBEDDER_DATA_INDEX, node::g_env);
  context->v8_context()->SetSecurityToken(g_context->GetSecurityToken());

  v8::Handle<v8::Object> nw = AsObjectOrEmpty(CreateNW(context, node_global, g_context));
#if 1
  v8::Local<v8::Array> symbols = v8::Array::New(isolate, 5);
  symbols->Set(0, v8::String::NewFromUtf8(isolate, "global"));
  symbols->Set(1, v8::String::NewFromUtf8(isolate, "process"));
  symbols->Set(2, v8::String::NewFromUtf8(isolate, "Buffer"));
  symbols->Set(3, v8::String::NewFromUtf8(isolate, "root"));
  symbols->Set(4, v8::String::NewFromUtf8(isolate, "require"));

  g_context->Enter();
  for (unsigned i = 0; i < symbols->Length(); ++i) {
    v8::Local<v8::Value> key = symbols->Get(i);
    v8::Local<v8::Value> val = node_global->Get(key);
    nw->Set(key, val);
  }
  g_context->Exit();
#endif
  if (node::g_dom_context.IsEmpty()) {
    node::g_dom_context.Reset(isolate, context->v8_context());
  }

  {
    blink::WebScopedMicrotaskSuppression suppression;
    v8::Context::Scope cscope(context->v8_context());
    // Make node's relative modules work
    std::string root_path = context->extension()->path().AsUTF8Unsafe();
#if defined(OS_WIN)
    base::ReplaceChars(root_path, "\\", "\\\\", &root_path);
#endif
    base::ReplaceChars(root_path, "'", "\\'", &root_path);
    v8::Local<v8::Script> script = v8::Script::Compile(v8::String::NewFromUtf8(isolate, (
        // Make node's relative modules work
        "if (typeof process != 'undefined' && (!process.mainModule.filename || process.mainModule.filename === 'blank')) {"
        "  var root = '" + root_path + "';"
#if defined(OS_WIN)
        "process.mainModule.filename = decodeURIComponent(window.location.pathname === 'blank' ? 'blank': window.location.pathname.substr(1));"
#else
        "process.mainModule.filename = root + '/index.html';"
#endif
        "if (window.location.href.indexOf('app://') === 0) {process.mainModule.filename = root + '/' + process.mainModule.filename}"
        "process.mainModule.paths = global.require('module')._nodeModulePaths(process.cwd());"
        "process.mainModule.loaded = true;"
        "}").c_str()),
    v8::String::NewFromUtf8(isolate, "process_main"));
    CHECK(*script);
    script->Run();
  }

}

void AmendManifestStringList(base::DictionaryValue* manifest,
                   const std::string& path,
                   const std::string& string_value) {
  base::ListValue* pattern_list = NULL;
  base::Value* temp_pattern_value = NULL;
  bool amend = false;

  if (manifest->Get(path, &temp_pattern_value))
      if (temp_pattern_value->GetAsList(&pattern_list))
        amend = true;
  if (!pattern_list)
    pattern_list = new base::ListValue();

  pattern_list->Append(new base::StringValue(string_value));
  if (!amend)
    manifest->Set(path, pattern_list);
}

void LoadNWAppAsExtensionHook(base::DictionaryValue* manifest, std::string* error) {
  if (!manifest)
    return;

  std::string main_url, bg_script, icon_path, node_remote;

  manifest->SetBoolean(manifest_keys::kNWJSFlag, true);
  if (error && !g_package->cached_error_content().empty()) {
    *error = g_package->cached_error_content();
    return;
  }

  if (manifest->GetString(manifest_keys::kNWJSMain, &main_url)) {
    AmendManifestStringList(manifest, manifest_keys::kPlatformAppBackgroundScripts, "nwjs/default.js");

    std::string bg_script;
    if (manifest->GetString("bg-script", &bg_script))
      AmendManifestStringList(manifest, manifest_keys::kPlatformAppBackgroundScripts, bg_script);

    AmendManifestStringList(manifest, manifest_keys::kPermissions, "developerPrivate");
    AmendManifestStringList(manifest, manifest_keys::kPermissions, "management");

  }
  if (manifest->GetString("window.icon", &icon_path)) {
    gfx::Image app_icon;
    if (g_package->GetImage(base::FilePath::FromUTF8Unsafe(icon_path), &app_icon)) {
      int width = app_icon.Width();
      std::string key = "icons." + base::IntToString(width);
      manifest->SetString(key, icon_path);
    }
  }
  if (manifest->GetString(switches::kmRemotePages, &node_remote)) {
    //FIXME: node-remote spec different with kWebURLs
    AmendManifestStringList(manifest, manifest_keys::kWebURLs, node_remote);
  }
}

} //namespace nw
