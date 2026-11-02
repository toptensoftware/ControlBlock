// Host unit tests for the firmware's platform-independent code.
// Run from this folder: `mk run` (needs `npm install -g toptensoftware/mk.js`).

export default async function ()
{
    await this.use("c-cpp");

    this.set({
        sourceFiles: (base) => [...base, ...this.otherSourceFiles],
        otherSourceFiles: [
            "../main/page_compositor.cpp",
            "../main/screen_view.cpp",
            "../main/status_bar.cpp",
            "../main/image_store.cpp",
            "../main/key_layout.cpp",
            "../main/protocol.cpp",
            "../main/touch_router.cpp",
            "../../rle565/src/rle565.c",
        ],
        includePath: [
            "../../SimpleLib",
            "../main",
            "../../rle565/src",
        ],
        define: [
            "_CRT_SECURE_NO_WARNINGS",
        ],
        gcc_cpp_args: [
            "-Wno-unused-variable",
        ],
        msvc_cl_args: [
            "/EHsc",            // Assert() throws
            "/wd4200",          // zero-sized arrays in rle565 headers
        ],
    });
};
