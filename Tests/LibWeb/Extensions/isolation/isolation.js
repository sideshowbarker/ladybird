{
    const root = document.documentElement;
    try {
        window.extensionSecret = 1;
        document.body.extensionExpando = 2;
        root.dataset.pageSecret = typeof window.pageSecret;
        root.dataset.pageExpando = typeof document.body.pageExpando;
        root.dataset.browser = typeof browser + " " + (browser === chrome);
        root.dataset.sameDom = String(document.body.getAttribute("data-marker") === "from-page");
    } catch (error) {
        root.dataset.error = String(error);
    }
}
