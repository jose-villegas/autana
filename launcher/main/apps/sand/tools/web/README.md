# Sand in a browser

```sh
./launcher/main/apps/sand/tools/web/setup_emsdk.sh   # once, unless emcc is on PATH
./launcher/main/apps/sand/tools/web/build_web.sh     # into web/dist/
python3 -m http.server -d launcher/main/apps/sand/tools/web/dist
```

`web/` compiles the simulation, the row painter and its clocks to
WebAssembly, with `web_sand.c` doing the app's frame and `app.js` the page's
input. The grid is the panel's own shape at the device's cell sizes;
landscape is that panel turned a quarter turn. The Pages workflow publishes
it under the site's `sand/` on every push to `main`, and builds it, without
publishing, for each pull request that changes a file
`build_web.sh --inputs` lists.
