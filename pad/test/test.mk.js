// Host unit tests for the macro pad code (pad/common, shared by the C3 firmware and the S3).
// Run from this folder: `mk run` (needs `npm install -g toptensoftware/mk.js`).

export default async function ()
{
    await this.use("c-cpp");

    this.set({
        // 32-bit, like firmware/test, so the shared code is checked in the S3's/C3's word size
        platform: "x86",
        sourceFiles: (base) => [...base, ...this.otherSourceFiles],
        otherSourceFiles: [
            "../common/pad_protocol.cpp",
            "../common/glitch_filter.cpp",
            "../common/quad_decoder.cpp",
            "../common/encoder_channel.cpp",
            "../common/pad_scanner.cpp",
            "../../PacketEncoder/packenc.c",
            "../../PacketEncoder/crc32.c",
        ],
        includePath: [
            "../../SimpleLib",
            "../common",
            "../../PacketEncoder",
        ],
        define: [
            "_CRT_SECURE_NO_WARNINGS",
        ],
        gcc_cpp_args: [
            "-Wno-unused-variable",
        ],
        msvc_cl_args: [
            "/EHsc",            // Assert() throws
        ],
    });
};
