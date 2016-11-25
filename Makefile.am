ACLOCAL_AMFLAGS = -I m4
AM_LDFLAGS = @BOOST_LDFLAGS@ @COVERAGE_LDFLAGS@
AM_CPPFLAGS = -I@abs_srcdir@/include -I@abs_srcdir@/include/proto @BOOST_CPPFLAGS@ -Igenfiles
AM_CXXFLAGS = -I@abs_srcdir@/include -I@abs_srcdir@/include/proto @COVERAGE_CXXFLAGS@ -Igenfiles
LIBTOOL_DEPS = @LIBTOOL_DEPS@
libtool: $(LIBTOOL_DEPS)
	$(SHELL) ./config.status libtool

# conditional test coverage
if ENABLE_COVERAGE
.PHONY: clean-coverage
clean-coverage:
	-find -name '*.gcda' -exec rm -rf {} \;
	-$(LCOV) --directory $(top_builddir) -z
	-rm -rf coverage.info coverage/

.PHONY: coverage-report
coverage-report: clean-coverage
	-$(MAKE) $(AM_MAKEFLAGS) -k check
	$(MAKE) $(AM_MAKEFLAGS) coverage/index.html

coverage.info:
	$(LCOV) --directory $(top_builddir) --base-directory $(top_builddir) --no-external --capture --output-file $@ --no-checksum --compat-libtool

coverage/index.html: coverage.info
	$(GENHTML) --prefix $(top_builddir) --output-directory $(@D) --title "Test Coverage" --legend --show-details $<


.PHONY: clean-gcno
clean-gcno:
	-find -name '*.gcno' -exec rm -rf {} \;

clean-local: clean-coverage clean-gcno clean-genfiles
else
clean-local: clean-genfiles
endif

clean-genfiles:
	-rm -rf genfiles

# protobuf generated code
PROTO_FILES := proto/segment.proto proto/tile.proto
src/proto/%.pb.cc: proto/%.proto
	@echo "PROTOC $<"; mkdir -p src/proto include/proto; @PROTOC_BIN@ -Iproto --cpp_out=include/proto $< && mv include/proto/$(@F) src/proto

#distributed executables
bin_PROGRAMS = osmlr
osmlr_SOURCES = src/osmlr.cpp src/proto/segment.pb.cc src/proto/tile.pb.cc src/output/output.cpp src/output/geojson.cpp src/output/tiles.cpp src/util/tile_writer.cpp
osmlr_CPPFLAGS = $(DEPS_CFLAGS) $(VALHALLA_DEPS_CFLAGS) @BOOST_CPPFLAGS@
osmlr_LDADD = $(DEPS_LIBS) $(VALHALLA_DEPS_LIBS) $(BOOST_PROGRAM_OPTIONS_LIB) $(BOOST_FILESYSTEM_LIB) $(BOOST_SYSTEM_LIB) $(BOOST_THREAD_LIB)

# tests
#check_PROGRAMS = test/something
#test_something_SOURCES = test/something.cpp test/test.cpp
#test_something_CPPFLAGS = $(DEPS_CFLAGS) $(VALHALLA_DEPS_CFLAGS) @BOOST_CPPFLAGS@
#test_something_LDADD = $(DEPS_LIBS) $(VALHALLA_DEPS_LIBS) @BOOST_LDFLAGS@ $(BOOST_PROGRAM_OPTIONS_LIB) $(BOOST_FILESYSTEM_LIB) $(BOOST_SYSTEM_LIB) $(BOOST_THREAD_LIB)

TESTS = $(check_PROGRAMS)
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = sh

test: check
