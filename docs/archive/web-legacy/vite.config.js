import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// The launcher is loaded from memory inside WebView2 (no server, no network),
// so the production build must be a single self-contained index.html.
// `--mode preview` builds the same page wired to the in-memory mock into
// dist-preview/ for screenshot capture; it is never embedded in the exe.
export default defineConfig(({ mode }) => ({
  plugins: [react(), viteSingleFile()],
  build: {
    outDir: mode === 'preview' ? 'dist-preview' : 'dist',
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
    chunkSizeWarningLimit: 10_000,
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
      },
    },
  },
}));
