// An extension page's content security policy allows no inline script, so this runs from its own file.
parent.postMessage({ title: document.title, origin: location.origin }, "*");
