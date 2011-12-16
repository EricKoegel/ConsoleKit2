NULL =

displaydir = $(sysconfdir)/ConsoleKit/displays.d
display_in_files = \
	Local.display.in \
	RemoteMachine.display.in \
	LocalVNC.display.in \
	Sunray.display.in \
	Headless.display.in

display_DATA = $(display_in_files:.display.in=.display)

Local.display: Local.display.in Makefile
	sed -e "s|\@X11_DIR\@|$(X11_DIR)|" $< > $@
RemoteMachine.display: RemoteMachine.display.in Makefile
	sed -e "s|\@X11_DIR\@|$(X11_DIR)|" $< > $@
LocalVNC.display: LocalVNC.display.in Makefile
	sed -e "s|\@X11_DIR\@|$(X11_DIR)|" $< > $@
Sunray.display: Sunray.display.in Makefile
	sed -e "s|\@X11_DIR\@|$(X11_DIR)|" $< > $@
Headless.display: Headless.display.in Makefile
	sed -e "s|\@X11_DIR\@|$(X11_DIR)|" $< > $@

EXTRA_DIST =						\
	$(display_in_files)				\
	$(NULL)

MAINTAINERCLEANFILES =			\
	*~				\
	Makefile.in

CLEANFILES = $(display_DATA)
