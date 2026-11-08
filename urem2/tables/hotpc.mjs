// The HOT PC (Wake-on-LAN to turn on; HTTP POST JSON {} to :4500/<command>) and the photo slideshow (UDP text
// commands and HTTP POST JSON {"<element>":"toggle"} to :4500/display-elements), as urem drove them.
// Both are PCs listening on :4500, so they share one module (and one HTTP listener in the test server).

export const HOTPC_MAC = "04:d4:c4:4e:da:2b";

// POST /<command> with the JSON body {}
export const HOTPC_COMMANDS = ["launch-steam", "launch-kiosk", "shutdown"];

// UDP text, one command per datagram
export const SLIDESHOW_COMMANDS = [
    "prevDay", "nextDay", "backward20", "forward20", "play", "pause", "voteUp", "voteDown", "prev", "next",
];

// POST /display-elements with displayElementsBody(element)
export const SLIDESHOW_ELEMENTS = ["all", "transport", "date", "time", "rating", "location", "other"];

export function displayElementsBody(element)
{
    if (!SLIDESHOW_ELEMENTS.includes(element))
        throw new Error(`unknown slideshow element ${JSON.stringify(element)}`);
    return JSON.stringify({ [element]: "toggle" });
}
