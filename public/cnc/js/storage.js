window.CNCApp = window.CNCApp || {};

CNCApp.Storage = {
  KEY: "CNC_DISPENSER_V20_STORAGE",

  saveAutoSave: function() {
    var tabsPayload = [];
    var activeId = null;

    if (CNCApp.Tabs) {
      tabsPayload = CNCApp.Tabs.exportTabsForStorage(false);
      activeId = CNCApp.Tabs.activeTabId;
    } else {
      tabsPayload = [{
        id: 'tab_default',
        projectName: CNCApp.State.projectName || 'Нов проект.csv',
        isDirty: !!CNCApp.State.isDirty,
        points: CNCApp.State.points || [],
        header: CNCApp.State.header || '',
        footer: CNCApp.State.footer,
        svgData: CNCApp.State.svgData || null
      }];
      activeId = 'tab_default';
    }

    var payload = {
      version: '2.0',
      activeTabId: activeId,
      tabs: tabsPayload,
      machine: CNCApp.State.machine,
      stepIncrement: CNCApp.State.stepIncrement
    };

    try {
      localStorage.setItem(this.KEY, JSON.stringify(payload));
    } catch (e) {
      // Заштита од надминување на 5MB localStorage лимит: зачувај без SVG на неактивните табови
      try {
        if (CNCApp.Tabs) {
          payload.tabs = CNCApp.Tabs.exportTabsForStorage(true);
          localStorage.setItem(this.KEY, JSON.stringify(payload));
        }
      } catch (e2) {}
    }
  },

  migrateMachineSettings: function(machine) {
    if (machine && Array.isArray(machine.dispensers)) {
      var d1 = machine.dispensers[0];
      if (d1 && (d1.offsetX === 0.8 || d1.offsetX === -0.4 || d1.offsetY === 36 || d1.offsetY === 34.55)) {
        machine.dispensers = [
          { id: 1, name: "Боја 1", color: "#2563eb", lowerCmd: "M104", raiseCmd: "M204", offsetX: -0.30, offsetY: 34.45 },
          { id: 2, name: "Боја 2", color: "#059669", lowerCmd: "M105", raiseCmd: "M205", offsetX: 39.70, offsetY: 34.45 },
          { id: 3, name: "Боја 3", color: "#dc2626", lowerCmd: "M106", raiseCmd: "M206", offsetX: 79.70, offsetY: 34.45 },
          { id: 4, name: "Боја 4", color: "#d97706", lowerCmd: "M107", raiseCmd: "M207", offsetX: 119.70, offsetY: 34.45 },
          { id: 5, name: "Боја 5", color: "#7c3aed", lowerCmd: "M108", raiseCmd: "M208", offsetX: 159.70, offsetY: 34.45 }
        ];
      }
      CNCApp.State.machine = machine;
      if (typeof CNCApp.State.normalizeMachineConfig === 'function') {
        CNCApp.State.normalizeMachineConfig();
      }
    }
  },

  migrateFooter: function(footerStr) {
    if (!footerStr || footerStr === 'G28.1 X2 Y2;M5;M30' || !footerStr.includes('G1 X1 Y1 F2000')) {
      return "G28.1 X2 Y2\r\nG1 X1 Y1 F2000\r\nM5\r\nM30";
    }
    return footerStr;
  },

  loadAutoSave: function() {
    try {
      var raw = localStorage.getItem(this.KEY)
        || localStorage.getItem("CNC_DISPENSER_V19_STORAGE")
        || localStorage.getItem("CNC_DISPENSER_V185_STORAGE")
        || localStorage.getItem("CNC_DISPENSER_V161_AUTOSAVE");
      if (!raw) return false;
      var data = JSON.parse(raw);

      if (data.machine) {
        this.migrateMachineSettings(data.machine);
      }
      if (data.stepIncrement) {
        CNCApp.State.stepIncrement = data.stepIncrement;
      }

      // Нов V2.0 мулти-таб формат
      if (Array.isArray(data.tabs) && data.tabs.length > 0 && CNCApp.Tabs) {
        for (var i = 0; i < data.tabs.length; i++) {
          data.tabs[i].footer = this.migrateFooter(data.tabs[i].footer);
        }
        CNCApp.Tabs.restoreFromStorage(data.tabs, data.activeTabId);
        return true;
      }

      // Наследен V1.x формат (еден проект)
      if (data.points && data.points.length > 0) {
        var migratedFooter = this.migrateFooter(data.footer);
        if (CNCApp.Tabs) {
          CNCApp.Tabs.restoreFromStorage([{
            id: 'tab_migrated',
            projectName: data.projectName || 'Нов проект.csv',
            isDirty: !!data.isDirty,
            points: data.points,
            header: data.header || '',
            footer: migratedFooter,
            svgData: data.svgData || null
          }], 'tab_migrated');
          return true;
        }

        CNCApp.State.points = data.points;
        CNCApp.State.header = data.header || '';
        CNCApp.State.footer = migratedFooter;
        if (data.projectName) CNCApp.State.projectName = data.projectName;
        CNCApp.State.isDirty = !!data.isDirty;
        if (data.svgData) CNCApp.Parser.loadSvgContent(data.svgData);
        if (typeof CNCApp.State.updateProjectUI === 'function') CNCApp.State.updateProjectUI();
        return true;
      }
    } catch (e) {}
    return false;
  }
};