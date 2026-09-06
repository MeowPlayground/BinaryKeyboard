<script setup lang="ts">
import { computed, ref } from 'vue';
import { useDeviceStore } from '@/stores/deviceStore';
import { showToast } from '@/services/toastService';
import { RgbMode } from '@/types/protocol';
import StudioSidebarEntry from '@/components/StudioSidebarEntry.vue';

const deviceStore = useDeviceStore();
const isSaving = ref(false);

const emit = defineEmits<{
  (e: 'open'): void;
}>();

const modeNames: Record<number, string> = {
  [RgbMode.OFF]: '关闭',
  [RgbMode.STATIC]: '静态',
  [RgbMode.BREATHING]: '呼吸',
  [RgbMode.BLINK]: '闪烁',
  [RgbMode.RAINBOW]: '彩虹',
  [RgbMode.INDICATOR]: '指示灯',
};

const rgbEnabled = computed(() => deviceStore.rgbConfig.enabled);
const modeLabel = computed(() => modeNames[deviceStore.rgbConfig.mode] ?? '未知');
const brightnessLabel = computed(() => `${Math.round(deviceStore.rgbConfig.brightness * 100 / 255)}%`);

async function toggleRgb(event: Event) {
  event.stopPropagation();
  if (isSaving.value) return;
  isSaving.value = true;
  try {
    deviceStore.rgbConfig.enabled = !deviceStore.rgbConfig.enabled;
    await deviceStore.saveRgbConfig();
  } catch (error) {
    showToast('error', 'RGB 保存失败', error instanceof Error ? error.message : '未知错误');
  } finally {
    isSaving.value = false;
  }
}
</script>

<template>
  <StudioSidebarEntry
    class="rgb-entry"
    title="灯光与电源"
    :meta="`${rgbEnabled ? modeLabel : '关闭'} · 亮度 ${brightnessLabel}`"
    icon="pi pi-palette"
    aria-label="打开灯光与电源设置"
    @open="emit('open')"
  >
    <template #action>
      <button
        type="button"
        class="rgb-switch"
        :class="{ active: rgbEnabled }"
        :disabled="isSaving"
        :aria-pressed="rgbEnabled"
        title="开关 RGB"
        @click="toggleRgb"
      >
        <span></span>
      </button>
    </template>
  </StudioSidebarEntry>
</template>

<style scoped>
.rgb-entry {
  --sidebar-entry-action-width: 2.65rem;
}

.rgb-switch {
  position: relative;
  width: 2.55rem;
  height: 1.45rem;
  border-radius: 999px;
  border: 1px solid var(--c-border);
  background: var(--c-bg-tertiary);
  cursor: pointer;
  transition: border-color 0.16s ease, background 0.16s ease, box-shadow 0.16s ease;
}

.rgb-switch span {
  position: absolute;
  top: 0.18rem;
  left: 0.18rem;
  width: 0.98rem;
  height: 0.98rem;
  border-radius: 50%;
  background: var(--c-text-muted);
  transition: transform 0.16s ease, background 0.16s ease;
}

.rgb-switch.active {
  border-color: var(--c-accent);
  background: var(--c-accent-soft);
  box-shadow: 0 0 16px color-mix(in srgb, var(--c-accent) 24%, transparent);
}

.rgb-switch.active span {
  transform: translateX(1.1rem);
  background: var(--c-accent-light);
}

.rgb-switch:disabled {
  opacity: 0.55;
  cursor: wait;
}
</style>
