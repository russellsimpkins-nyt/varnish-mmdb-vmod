ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src tests
docdir = ${datadir}/doc/${PACKAGE}
doc_DATA = README.md LICENSE

dist_man_MANS = vmod_geo.3
MAINTAINERCLEANFILES = $(dist_man_MANS)

vmod_geo.3: README.md

%.1 %.2 %.3 %.4 %.5 %.6 %.7 %.8 %.9:
if HAVE_RST2MAN
	${RST2MAN} $< $@
else
	@echo "========================================"
	@echo "You need rst2man installed to make dist"
	@echo "========================================"
	@false
endif

test:
	tests/testrunner
