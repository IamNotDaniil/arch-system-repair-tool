# Maintainer: Your Name <email@example.com>
pkgname=arch-system-repair-tool-git
pkgver=2.3
pkgrel=1
pkgdesc="Low-level UEFI/BIOS recovery tool for Arch Linux"
arch=('x86_64')
url="https://github.com/yourusername/arch-system-repair-tool"
license=('GPL3')
depends=('glibc')
makedepends=('git' 'gcc' 'make')
source=("git+https://github.com/yourusername/arch-system-repair-tool.git")
sha256sums=('SKIP')

build() {
    cd "$srcdir/arch-system-repair-tool"
    make
}

package() {
    cd "$srcdir/arch-system-repair-tool"
    make DESTDIR="$pkgdir" install
}
