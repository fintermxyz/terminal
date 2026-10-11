// @ts-check
import { defineConfig } from 'astro/config';

// Plain static output, served by Cloudflare Workers static assets (see wrangler.jsonc).
export default defineConfig({
  output: 'static',
  build: {
    // one small stylesheet: inline it so the page is a single request
    inlineStylesheets: 'always',
  },
});
