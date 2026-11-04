// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The page-side half of the script message bridge, shared by every backend.
// Keeping it in one translation unit means the mock and the real engine expose
// byte-identical semantics to the page, so a page written against the mock
// keeps working once the engine is linked in.

#include "webgtk_backend_ops.h"

namespace webgtk {

// The guard makes re-injection idempotent: backends install this on every
// document, and a document may be reached more than once (restore, bfcache).
const char kWebGtkBridgeScript[] =
    "(function () {\n"
    "  if (window.webgtk && window.webgtk.__webgtk_bridge) return;\n"
    "  window.webgtk = {\n"
    "    __webgtk_bridge: true,\n"
    "    postMessage: function (name, payload) {\n"
    "      if (typeof payload === 'object' && payload !== null) {\n"
    "        payload = JSON.stringify(payload);\n"
    "      }\n"
    "      if (typeof window.__webgtk_internal_post === 'function') {\n"
    "        window.__webgtk_internal_post(String(name),\n"
    "                                      String(payload == null ? '' "
    ": payload));\n"
    "      }\n"
    "    }\n"
    "  };\n"
    "})();\n";

}  // namespace webgtk
