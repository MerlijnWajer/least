# Copyright 1999-2013 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

EAPI=4

inherit eutils git-2

DESCRIPTION="Least, a not so minimalistic PDF viewer"
HOMEPAGE="http://wizzup.org/least"
SRC_URI=""

EGIT_REPO_URI="git://github.com/MerlijnWajer/least.git"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64 ~arm ~x86"
IUSE=""

DEPEND=">=app-text/mupdf-1.2"
RDEPEND="${DEPEND}"

src_unpack() {
	git-2_src_unpack
}

src_compile() {
	emake
}

src_install() {
	dobin least
}

