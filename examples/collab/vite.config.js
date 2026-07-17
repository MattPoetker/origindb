import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  build: {
    outDir: "public",
    emptyOutDir: true,
  },
  server: {
    // `npm run dev` proxies API calls to the running demo server
    proxy: {
      "/api": "http://localhost:9090",
    },
  },
});
