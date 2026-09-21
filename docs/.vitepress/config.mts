import { defineConfig } from "vitepress";
import { withMermaid } from "vitepress-plugin-mermaid";

export default withMermaid(
  defineConfig({
    title: "WebDB Internals",
    description:
      "Architectural Handbook for WebDB — In-Browser Relational Database Engine",
    base: process.env.GITHUB_ACTIONS ? "/webdb.cpp/" : "/",
    srcExclude: ["**/fixed-issues.md"],
    ignoreDeadLinks: true,
    themeConfig: {
      siteTitle: "WebDB Handbook",
      nav: [
        { text: "Handbook", link: "/webdb_internals_handbook" },
        {
          text: "Phases",
          items: [
            {
              text: "Phase 1: Storage Format",
              link: "/phase1_storage_format_guide",
            },
            {
              text: "Phase 2: Async Scheduler",
              link: "/phase2_async_scheduler_guide",
            },
            { text: "Phase 3: Buffer Pool", link: "/phase3_buffer_pool_guide" },
          ],
        },
        { text: "GitHub", link: "https://github.com/ahmad-moussawi/webdb" },
      ],

      sidebar: [
        {
          text: "Internals Handbook",
          items: [
            {
              text: "Architectural Overview",
              link: "/webdb_internals_handbook",
            },
            {
              text: "Phase 1: Storage & Slotted Pages",
              link: "/phase1_storage_format_guide",
            },
            {
              text: "Phase 2: Host-Driven Async Scheduler",
              link: "/phase2_async_scheduler_guide",
            },
            {
              text: "Phase 3: Buffer Pool Manager",
              link: "/phase3_buffer_pool_guide",
            },
          ],
        },
      ],

      socialLinks: [
        { icon: "github", link: "https://github.com/ahmad-moussawi/webdb" },
      ],

      footer: {
        message: "Published under WebDB open-source license.",
        copyright: "Copyright © 2026 WebDB Contributors",
      },

      search: {
        provider: "local",
      },
    },
  }),
);
