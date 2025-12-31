```sh
rm -fr build
meson setup build --prefix=/usr --libdir=lib/x86_64-linux-gnu
ninja -C build
sudo ninja -C build install
xfce4-panel --restart
```
