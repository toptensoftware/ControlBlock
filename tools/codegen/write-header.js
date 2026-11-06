// Shared by the gen-*.js writers: write one generated header and say so.

import fs from "node:fs";

export function writeHeader(file, text)
{
    fs.writeFileSync(file, text);
    console.log(`${file}: written`);
}
