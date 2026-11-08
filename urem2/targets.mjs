// Where urem2's devices are: `real` (the house) and `test` (urem2/test-server on the hardware test PC).
// The layout picks one set at bake time; the test server takes its default ports from `test`, so the two agree.

import { SERVER } from "../examples/test-common.mjs";
import { IRTX_PORT } from "./tables/irtx.mjs";
import { ATV_PORT } from "./tables/appletv.mjs";
import { YAMAHA_API } from "./tables/yamaha.mjs";
import { HOTPC_MAC } from "./tables/hotpc.mjs";

const PC_PORT = 4500;               // the HOT PC's and the slideshow's HTTP (and the slideshow's UDP)

export const targets = {
    real: {
        irtx: { host: "10.1.1.188", port: IRTX_PORT },
        atvd: { host: "10.1.1.2", port: ATV_PORT },                    // cool.lan (the NAS); IP, no DNS dependency
        yamaha: "http://yama:80" + YAMAHA_API,                         // may need yama.lan (lwIP and DHCP domains)
        hotpc: { host: "10.1.1.105", port: PC_PORT, mac: HOTPC_MAC },
        slideshow: { host: "10.1.1.2", udpPort: PC_PORT, httpPort: PC_PORT },
    },
    test: {
        irtx: { host: SERVER, port: IRTX_PORT },
        atvd: { host: SERVER, port: ATV_PORT },
        yamaha: `http://${SERVER}:8080${YAMAHA_API}`,                  // Windows' port 80 is often taken
        hotpc: { host: SERVER, port: PC_PORT, mac: HOTPC_MAC },
        // UDP 4501, not 4500: Windows drops inbound UDP from other machines on 4500 (IPsec NAT-T)
        slideshow: { host: SERVER, udpPort: PC_PORT + 1, httpPort: PC_PORT },
    },
};
