# SysMonitor — 透明桌面系统监视器
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
PKG    := x11 freetype2
INCS   := $(shell pkg-config --cflags $(PKG))
LIBS   := $(shell pkg-config --libs $(PKG))
BIN    := sysmonitor
DEST   ?= /usr/local/bin

$(BIN): sysmonitor.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(DEST)/$(BIN)
	@echo "已安装到 $(DEST)/$(BIN)"

autostart: install
	@mkdir -p $(HOME)/.config/autostart
	@sed 's|EXEC_PATH|$(DEST)/$(BIN)|' sysmonitor.desktop.in > $(HOME)/.config/autostart/sysmonitor.desktop
	@echo "已创建开机自启动项: $(HOME)/.config/autostart/sysmonitor.desktop"

uninstall:
	rm -f $(DEST)/$(BIN) $(HOME)/.config/autostart/sysmonitor.desktop
	@echo "已卸载"

clean:
	rm -f $(BIN)

.PHONY: install autostart uninstall clean
