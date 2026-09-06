<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue';
import { useDeviceStore } from '@/stores/deviceStore';
import { useConfirm } from 'primevue/useconfirm';
import { useMacroStore } from '@/stores/macroStore';
import { useTerminalStore } from '@/stores/terminalStore';
import { showToast } from '@/services/toastService';
import type { KeyAction } from '@/types/protocol';
import { createEmptyAction, DeviceProtocol, KeyboardType, KeyboardTypeInfo } from '@/types/protocol';
import { createDeviceUiDefinition, hasUiSection } from '@/types/deviceUi';
import { getHidDevicePlugin } from '@/services/hid/registry';

import AppHeader from '@/layouts/AppHeader.vue';
import CatAssistant from '@/components/CatAssistant.vue';
import KeyboardLayout from '@/components/KeyboardLayout.vue';
import KeyboardDecoration from '@/components/KeyboardDecoration.vue';
import StudioDialog from '@/components/StudioDialog.vue';
import ActionEditor from '@/components/ActionEditor.vue';
import MacroEditor from '@/components/MacroEditor.vue';
import DeviceInfoPanel from '@/components/DeviceInfoPanel.vue';
import KeyboardStatus from '@/components/KeyboardStatus.vue';
import LayerPanel from '@/components/LayerPanel.vue';
import FnPanel from '@/components/FnPanel.vue';
import RgbPanel from '@/components/RgbPanel.vue';
import RgbEntry from '@/components/RgbEntry.vue';
import ConfigLibraryEntry from '@/components/ConfigLibraryEntry.vue';
import ConfigLibraryPanel from '@/components/ConfigLibraryPanel.vue';
import MacroPanel from '@/components/MacroPanel.vue';
import StormDataFlashEntry from '@/components/StormDataFlashEntry.vue';
import StormDataFlashPanel from '@/components/StormDataFlashPanel.vue';
import ThemeConfigurator from '@/components/ThemeConfigurator.vue';
import FallingEffect from '@/components/FallingEffect.vue';
import CatEars from '@/components/CatEars.vue';
import VideoBackground from '@/components/VideoBackground.vue';
import { useTheme } from '@/composables/useTheme';

const props = defineProps<{
  onRefresh: () => void;
  onDisconnect: () => void;
  onToggleTheme: () => void;
}>();

const confirm = useConfirm();
const deviceStore = useDeviceStore();
const macroStore = useMacroStore();
const terminalStore = useTerminalStore();
const { openConfigurator, themeId } = useTheme();

// 预览模式
const previewKeyboardType = ref(-1);

// 编辑器状态
const editorVisible = ref(false);
const selectedKeyIndex = ref(-1);
const macroEditorVisible = ref(false);
const macroEditorSlot = ref(0);
const dataFlashVisible = ref(false);
const configLibraryVisible = ref(false);
const configLibraryRefreshKey = ref(0);
const deviceInfoVisible = ref(false);
const rgbPanelVisible = ref(false);
const kbCardRef = ref<HTMLElement | null>(null);
const earStyle = ref<Record<string, string>>({});

function updateEarPosition() {
  if (!kbCardRef.value) return;
  const rect = kbCardRef.value.getBoundingClientRect();
  earStyle.value = {
    top: `${rect.top - 50}px`,
    left: `${rect.left + rect.width / 2}px`,
    transform: 'translateX(-50%)',
  };
}

let earRaf = 0;
function earLoop() {
  updateEarPosition();
  earRaf = requestAnimationFrame(earLoop);
}

onMounted(() => { if (themeId.value === 'neko') earLoop(); });
onUnmounted(() => {
  cancelAnimationFrame(earRaf);
});

watch(themeId, (id) => {
  if (id === 'neko') { earLoop(); } else { cancelAnimationFrame(earRaf); }
});

watch(configLibraryVisible, (visible) => {
  if (!visible) configLibraryRefreshKey.value += 1;
});

watch(previewKeyboardType, (value) => {
  if (value >= 0) {
    deviceStore.setEditLayer(0);
    showToast(
      'info',
      '预览模式',
      `正在预览 ${KeyboardTypeInfo[value as KeyboardType]?.name || '未知型号'}`,
    );
    return;
  }

  if (deviceStore.deviceInfo) {
    deviceStore.setEditLayer(deviceStore.keymap.currentLayer);
    showToast('info', '实际设备', '已切回当前连接的键盘');
  }
});

async function openMacroEditor(slot: number) {
  await macroStore.startEditing(slot);
  macroEditorSlot.value = slot;
  macroEditorVisible.value = true;
}

const selectedAction = computed<KeyAction>(() => {
  if (selectedKeyIndex.value < 0) return createEmptyAction();
  return deviceStore.getKeyAction(selectedKeyIndex.value) || createEmptyAction();
});

const keyboardSectionStyle = computed(() => {
  const statusBarH = 32;
  if (terminalStore.isOpen) {
    return { bottom: (terminalStore.panelHeight + statusBarH) + 'px' };
  }
  return { bottom: statusBarH + 'px' };
});

const currentKeyboardType = computed(() => {
  if (previewKeyboardType.value >= 0) return previewKeyboardType.value;
  return deviceStore.deviceInfo?.keyboardType ?? 0;
});

const currentUiDefinition = computed(() => {
  if (previewKeyboardType.value >= 0) {
    return createDeviceUiDefinition('preview', ['device-info', 'keyboard-status', 'layer-panel', 'actions-panel', 'debug-terminal']);
  }
  const protocol = deviceStore.deviceInfo?.protocol;
  if (!protocol) {
    return createDeviceUiDefinition('preview', ['device-info', 'keyboard-status', 'actions-panel', 'debug-terminal']);
  }
  const plugin = getHidDevicePlugin(protocol);
  if (!plugin) {
    return createDeviceUiDefinition(protocol, ['device-info', 'keyboard-status', 'actions-panel', 'debug-terminal']);
  }
  return plugin.getUiDefinition(deviceStore.capabilities);
});

const showLayerPanel = computed(() => hasUiSection(currentUiDefinition.value, 'layer-panel'));
const showFnPanel = computed(() => previewKeyboardType.value < 0 && hasUiSection(currentUiDefinition.value, 'fn-panel'));
const showRgbPanel = computed(() => previewKeyboardType.value < 0 && hasUiSection(currentUiDefinition.value, 'rgb-panel'));
const showMacroPanel = computed(() => previewKeyboardType.value < 0 && deviceStore.supportsMacroActions);
const showConfigLibrary = computed(() => previewKeyboardType.value < 0);
const showResetButton = computed(() => previewKeyboardType.value < 0 && deviceStore.supportsFactoryReset);
const showLayerBadge = computed(() => previewKeyboardType.value >= 0 || deviceStore.supportsMultiLayer);
const showStormDataFlashPanel = computed(
  () =>
    themeId.value === 'storm' &&
    previewKeyboardType.value < 0 &&
    deviceStore.deviceInfo?.protocol === DeviceProtocol.CH592,
);
const saveKeymapLabel = computed(() => deviceStore.supportsExplicitSave ? '保存配置' : '写入键位');

const currentLayerKeysForDisplay = computed(() => {
  if (previewKeyboardType.value >= 0) {
    const keyCount = KeyboardTypeInfo[currentKeyboardType.value as KeyboardType]?.keys || 4;
    return Array.from({ length: keyCount }, () => createEmptyAction());
  }
  return deviceStore.currentLayerKeys;
});

// 配置操作
async function saveConfig() {
  try {
    await deviceStore.saveKeymap();
    showToast('success', '保存成功', deviceStore.supportsExplicitSave ? '配置已保存到设备' : '键位已写入设备');
  } catch (error) {
    showToast('error', '保存失败', error instanceof Error ? error.message : '未知错误');
  }
}

function discardChanges() {
  deviceStore.discardChanges();
  showToast('info', '已撤销', '更改已放弃');
}

function confirmReset() {
  confirm.require({
    message: '确定要恢复出厂设置吗？所有自定义配置将丢失。',
    header: '恢复出厂设置',
    icon: 'pi pi-exclamation-triangle',
    acceptClass: 'p-button-danger',
    acceptLabel: '确定重置',
    rejectLabel: '取消',
    accept: async () => {
      try {
        await deviceStore.resetToFactory();
        showToast('success', '重置成功', '已恢复出厂设置');
      } catch (error) {
        showToast('error', '重置失败', error instanceof Error ? error.message : '未知错误');
      }
    },
  });
}

function onKeySelect(index: number) {
  selectedKeyIndex.value = index;
  editorVisible.value = true;
}

function onActionSave(action: KeyAction) {
  if (selectedKeyIndex.value >= 0) {
    deviceStore.setKeyAction(selectedKeyIndex.value, action);
  }
  editorVisible.value = false;
}

function onCatAction(action: string) {
  switch (action) {
    case 'refresh': props.onRefresh(); break;
    case 'theme': props.onToggleTheme(); break;
    case 'themeConfig': openConfigurator(); break;
    case 'disconnect': props.onDisconnect(); break;
    case 'factoryReset': confirmReset(); break;
    case 'scrollTop': window.scrollTo({ top: 0, behavior: 'smooth' }); break;
  }
}
</script>

<template>
  <div class="main-layout">
    <!-- 安哥拉兔主题视频背景（lazy加载） -->
    <VideoBackground v-if="themeId === 'angora'" src="https://file.icve.com.cn/file_doc/qdqqd/WV62vskP_vip.mp4" />
    <!-- 琉璃主题飘落效果 -->
    <FallingEffect v-if="themeId === 'liuli'" type="sakura" :count="15" />
    <!-- 安哥拉兔主题飘落效果 -->
    <FallingEffect v-if="themeId === 'angora'" type="sakura" :count="10" />
    <!-- 青蛙主题飘落花瓣 -->
    <FallingEffect v-if="themeId === 'frog'" type="sakura" :count="10" />

    <CatAssistant
      v-model:preview-type="previewKeyboardType"
      :show-factory-reset="showResetButton"
      @action="onCatAction"
    />

    <AppHeader
      v-model:preview-type="previewKeyboardType"
      @refresh="onRefresh"
      @disconnect="onDisconnect"
      @open-device-info="deviceInfoVisible = true"
    />

    <main class="app-main" :class="{ 'terminal-open': terminalStore.isOpen }">
      <!-- 左侧面板 -->
      <aside class="sidebar">
        <KeyboardStatus />
        <LayerPanel v-if="showLayerPanel" :keyboard-type="currentKeyboardType" :preview-mode="previewKeyboardType >= 0" />
        <FnPanel v-if="showFnPanel" />
        <RgbEntry v-if="showRgbPanel" @open="rgbPanelVisible = true" />
        <ConfigLibraryEntry v-if="showConfigLibrary" :refresh-key="configLibraryRefreshKey" @open="configLibraryVisible = true" />
        <MacroPanel v-if="showMacroPanel" @edit="openMacroEditor" />
        <StormDataFlashEntry v-if="showStormDataFlashPanel" @open="dataFlashVisible = true" />
      </aside>

      <div class="keyboard-spacer"></div>

      <!-- 中央键盘区 -->
      <section class="keyboard-section" :style="keyboardSectionStyle">
        <KeyboardDecoration />

        <!-- 猫咪主题猫耳（独立浮层，不侵入键盘组件） -->
        <div v-if="themeId === 'neko'" class="cat-ears-overlay" :style="earStyle">
          <CatEars />
        </div>

        <div ref="kbCardRef" class="keyboard-card">
          <div class="card-header">
            <div class="card-header-row">
              <div class="card-title-wrap">
                <div class="card-title-section">
                  <span class="card-title">🎹 键盘布局</span>
                  <span v-if="showLayerBadge" class="card-layer-badge">层 {{ deviceStore.currentEditLayer + 1 }}</span>
                </div>
              </div>
              <div v-if="previewKeyboardType < 0" class="keyboard-actions">
                <button
                  type="button"
                  class="keyboard-action-btn save"
                  :disabled="!deviceStore.hasChanges"
                  :title="saveKeymapLabel"
                  @click="saveConfig"
                >
                  <i class="pi pi-save"></i>
                  <span>{{ saveKeymapLabel }}</span>
                </button>
                <button
                  type="button"
                  class="keyboard-action-btn"
                  :disabled="!deviceStore.hasChanges"
                  title="放弃更改"
                  @click="discardChanges"
                >
                  <i class="pi pi-undo"></i>
                  <span>放弃</span>
                </button>
              </div>
            </div>
          </div>
          <div class="keyboard-container">
            <KeyboardLayout :keyboard-type="currentKeyboardType" :keys="currentLayerKeysForDisplay"
              :selected-index="selectedKeyIndex" :disabled="previewKeyboardType >= 0"
              @select="onKeySelect" />
          </div>
        </div>

        <div v-if="deviceStore.hasChanges" class="changes-indicator">
          <i class="pi pi-exclamation-circle"></i>
          <span>有未保存的更改</span>
        </div>
      </section>
    </main>

    <ActionEditor v-model:visible="editorVisible" :key-index="selectedKeyIndex" :action="selectedAction"
      @save="onActionSave" />
    <StudioDialog
      v-model:visible="deviceInfoVisible"
      size="sm"
      header="设备信息"
      class="device-info-dialog"
    >
      <DeviceInfoPanel />
    </StudioDialog>
    <StudioDialog
      v-if="showRgbPanel"
      v-model:visible="rgbPanelVisible"
      size="sm"
      header="灯光与电源"
      class="rgb-dialog"
    >
      <RgbPanel />
    </StudioDialog>
    <ConfigLibraryPanel v-if="showConfigLibrary" v-model:visible="configLibraryVisible" />
    <MacroEditor v-model:visible="macroEditorVisible" :slot="macroEditorSlot" />
    <StormDataFlashPanel v-if="showStormDataFlashPanel" v-model:visible="dataFlashVisible" />
    <ThemeConfigurator />
  </div>
</template>

<style scoped>
.main-layout {
  display: flex;
  flex-direction: column;
  height: 100vh;
  overflow: hidden;
}

.app-main {
  flex: 1;
  display: flex;
  padding: 1.5rem;
  padding-top: calc(var(--header-height) + 1.5rem);
  padding-bottom: 50px;
  gap: clamp(1rem, 1.5vw, 1.5rem);
  overflow: hidden;
  transition: padding-bottom 0.3s ease;
}

.app-main.terminal-open {
  padding-bottom: 340px;
}

.sidebar {
  width: var(--sidebar-width);
  display: flex;
  flex-direction: column;
  gap: 0.65rem;
  flex-shrink: 0;
  height: calc(100vh - var(--header-height) - 3rem);
  overflow-y: auto;
  overflow-x: hidden;
  scrollbar-width: thin;
  scrollbar-color: var(--c-border) transparent;
}

.sidebar::-webkit-scrollbar { width: 4px; }
.sidebar::-webkit-scrollbar-track { background: transparent; }
.sidebar::-webkit-scrollbar-thumb { background: var(--c-border); border-radius: 2px; }

:deep(.device-info-dialog .panel),
:deep(.rgb-dialog .panel) {
  border: 0;
  background: transparent;
  padding: 0;
  box-shadow: none;
}

.keyboard-spacer {
  flex: 1;
  min-width: 200px;
}

.keyboard-section {
  position: fixed;
  left: calc(var(--sidebar-width) + clamp(2rem, 3vw, 3rem));
  right: 0;
  top: var(--header-height);
  bottom: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  pointer-events: none;
}

.keyboard-section .keyboard-card,
.keyboard-section .changes-indicator {
  pointer-events: auto;
}

.keyboard-card {
  position: relative;
  z-index: 1;
  background: var(--c-bg-secondary);
  border: 1px solid var(--c-border);
  border-radius: var(--radius-xl);
  overflow: hidden;
}

.card-header {
  background: var(--c-bg-tertiary);
  border-bottom: 1px solid var(--c-border);
  padding: 1rem 1.5rem;
}

.card-header-row {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
}

.card-title-wrap {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  min-width: 0;
}

.card-title-section {
  display: flex;
  align-items: center;
  gap: 0.75rem;
}

.card-title {
  font-size: 1rem;
  font-weight: 700;
  color: var(--c-text-primary);
}

.card-layer-badge {
  font-size: 0.75rem;
  font-weight: 700;
  color: var(--c-accent);
  background: var(--c-accent-soft);
  padding: 0.25rem 0.6rem;
  border-radius: var(--radius-sm);
  border: 1px solid var(--c-accent);
}

.keyboard-actions {
  display: inline-flex;
  align-items: center;
  gap: 0.45rem;
  flex-shrink: 0;
}

.keyboard-action-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 0.35rem;
  min-height: 2rem;
  padding: 0 0.7rem;
  border: 1px solid var(--c-border);
  border-radius: var(--radius-sm);
  background: var(--c-bg-secondary);
  color: var(--c-text-secondary);
  font: inherit;
  font-size: 0.75rem;
  font-weight: 800;
  cursor: pointer;
  transition: border-color 0.16s ease, background 0.16s ease, color 0.16s ease, transform 0.16s ease;
}

.keyboard-action-btn .pi {
  font-size: 0.78rem;
}

.keyboard-action-btn:hover:not(:disabled) {
  transform: translateY(-1px);
  border-color: var(--c-accent);
  color: var(--c-text-primary);
  background: var(--c-bg-hover);
}

.keyboard-action-btn.save {
  border-color: color-mix(in srgb, var(--c-accent) 62%, var(--c-border));
  color: var(--c-accent-light);
  background: var(--c-accent-soft);
}

.keyboard-action-btn:disabled {
  opacity: 0.42;
  cursor: default;
}

.keyboard-container {
  padding: 2rem 2.5rem;
}

.changes-indicator {
  margin-top: 1.25rem;
  display: flex;
  align-items: center;
  gap: 0.5rem;
  color: var(--c-warning);
  font-size: 0.9rem;
  font-weight: 600;
  padding: 0.5rem 1rem;
  background: rgba(251, 191, 36, 0.1);
  border-radius: var(--radius-md);
  position: relative;
  z-index: 1;
}

/* 猫耳独立浮层 — fixed 定位，JS 动态跟踪 keyboard-card 顶部 */
.cat-ears-overlay {
  position: fixed;
  z-index: 2;
  pointer-events: none;
}

@media (min-width: 1440px) {
  .sidebar {
    gap: 0.55rem;
  }
}

@media (max-width: 900px) {
  .card-header-row {
    flex-direction: column;
  }

  .keyboard-actions {
    width: 100%;
  }

  .keyboard-action-btn {
    flex: 1;
  }
}
</style>
