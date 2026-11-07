import { answer } from "./helper.js";

export function describe() {
    return {
        browser: typeof browser,
        pageSecret: typeof window.pageSecret,
        helper: answer,
        metaUrl: import.meta.url.split("/").slice(-2).join("/"),
    };
}
