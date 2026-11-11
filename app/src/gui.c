/* optimzr-gui: Gentoo Power Control window. Runs as the user and only talks
 * to optimzrd over its socket. */
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "proto.h"

static int fd = -1;
static GtkWidget *win, *scale, *pct_label, *summary, *net_drop, *net_summary, *auto_sw,
    *auto_note, *conn_label, *caps_label, *stock_btn, *body;
static GtkWidget *row_val[8];
static GtkWidget *cpu_bar, *gpu_bar, *ram_bar;
static int updating;  /* set while we move widgets ourselves */
static guint apply_id;
static char level_sum[6][160];
static char net_sum[3][160];
static const char *net_ids[] = {"auto", "balanced", "lowlatency", "throughput"};

static int call(const char *req, struct strbuf *ans)
{
    if (fd < 0)
        fd = proto_connect();
    if (fd < 0)
        return -1;
    int r = proto_call(fd, req, ans);
    if (r < 0) {
        close(fd);
        fd = -1;
    }
    return r;
}

static double num(const struct strbuf *a, const char *k)
{
    char b[64];
    return proto_get(a, k, b, sizeof b) ? atof(b) : -1;
}

static void set_text(GtkWidget *w, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_text(GtkWidget *w, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(w), b);
}

static void temp(char *out, size_t sz, double t)
{
    if (t < 0)
        snprintf(out, sz, "–");
    else
        snprintf(out, sz, "%.0f °C", t);
}

/* ------------------------------------------------------------- status */

static void load_levels(const struct strbuf *a)
{
    int li = 0, ni = 0;
    for (const char *p = a->s; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        int pct;
        char text[160];
        if (sscanf(p, "level=%d|%159[^\n]", &pct, text) == 2 && li < 6)
            snprintf(level_sum[li++], sizeof level_sum[0], "%s", text);
        else if (sscanf(p, "netprof=%*[^|]|%159[^\n]", text) == 1 && ni < 3)
            snprintf(net_sum[ni++], sizeof net_sum[0], "%s", text);
    }
}

static void show_summary(int pct)
{
    int i = (pct - 50) / 10;
    if (pct == 0) {
        set_text(pct_label, "Stock");
        set_text(summary, "Nothing is changed. Move the slider to manage this PC.");
        return;
    }
    set_text(pct_label, "%d%%", pct);
    if (i >= 0 && i < 6)
        set_text(summary, "%s", level_sum[i]);
}

static void refresh_status(void)
{
    struct strbuf a;
    if (call("status", &a))
        return;
    load_levels(&a);
    int base = (int)num(&a, "profile"), active = (int)num(&a, "active");
    char net[32] = "auto", nact[32] = "", work[16] = "idle";
    proto_get(&a, "net", net, sizeof net);
    proto_get(&a, "net_active", nact, sizeof nact);
    proto_get(&a, "workload", work, sizeof work);
    int au = num(&a, "auto") > 0;

    updating = 1;
    if (!apply_id && base >= 50)
        gtk_range_set_value(GTK_RANGE(scale), base);
    show_summary(apply_id ? (int)gtk_range_get_value(GTK_RANGE(scale)) : base);
    for (unsigned i = 0; i < G_N_ELEMENTS(net_ids); i++)
        if (!strcmp(net, net_ids[i]))
            gtk_drop_down_set_selected(GTK_DROP_DOWN(net_drop), i);
    gtk_switch_set_active(GTK_SWITCH(auto_sw), au);
    updating = 0;

    int ni = !strcmp(nact, "balanced") ? 0 : !strcmp(nact, "lowlatency") ? 1 : !strcmp(nact, "throughput") ? 2 : -1;
    if (ni >= 0)
        set_text(net_summary, "%s%s", !strcmp(net, "auto") ? "Now: " : "", net_sum[ni]);
    else
        set_text(net_summary, " ");

    if (!au)
        set_text(auto_note, "Off. Your level stays as set.");
    else if (!strcmp(work, "idle"))
        set_text(auto_note, "On. Nothing running that needs more; at your level.");
    else
        set_text(auto_note, "On. %s detected, running at %d%%.", !strcmp(work, "game") ? "A game" : "A build",
                 active);
}

/* ------------------------------------------------------------ monitor */

static void refresh_monitor(void)
{
    struct strbuf a;
    char t1[32], t2[32], t3[32], b[256];
    if (call("monitor", &a)) {
        gtk_widget_set_sensitive(body, FALSE);
        set_text(conn_label, "optimzrd is not running (or you are not in its group). Retrying…");
        return;
    }
    gtk_widget_set_sensitive(body, TRUE);
    set_text(conn_label, " ");

    double use = num(&a, "cpu_use");
    temp(t1, sizeof t1, num(&a, "cpu_temp"));
    set_text(row_val[0], "%.2f GHz avg, %.2f max   %3.0f%%   %s   %.0f W", num(&a, "cpu_mhz") / 1000,
             num(&a, "cpu_mhz_max") / 1000, use, t1, num(&a, "cpu_w"));
    gtk_level_bar_set_value(GTK_LEVEL_BAR(cpu_bar), use > 0 ? use / 100 : 0);

    double gmhz = num(&a, "gpu_mhz"), guse = num(&a, "gpu_use");
    temp(t1, sizeof t1, num(&a, "gpu_temp"));
    temp(t2, sizeof t2, num(&a, "gpu_hot"));
    if (gmhz < 10)
        snprintf(b, sizeof b, "asleep");
    else
        snprintf(b, sizeof b, "%.0f MHz", gmhz);
    set_text(row_val[1], "%s   %3.0f%%   %s (hotspot %s)   %.0f / %.0f W", b, guse, t1, t2,
             num(&a, "gpu_w"), num(&a, "gpu_cap_w"));
    gtk_level_bar_set_value(GTK_LEVEL_BAR(gpu_bar), guse > 0 ? guse / 100 : 0);
    temp(t1, sizeof t1, num(&a, "gpu_mem_temp"));
    set_text(row_val[2], "%.1f / %.1f GB   %s   %.0f MHz", num(&a, "vram_used") / 1024,
             num(&a, "vram_total") / 1024, t1, num(&a, "gpu_mem_mhz"));

    double ru = num(&a, "ram_used"), rt = num(&a, "ram_total");
    set_text(row_val[3], "%.1f / %.1f GB   swap %.1f GB", ru / 1024, rt / 1024, num(&a, "swap_used") / 1024);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(ram_bar), rt > 0 ? ru / rt : 0);

    /* fans: "fan=label|rpm" lines */
    b[0] = 0;
    for (const char *p = a.s; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        char lab[32];
        double rpm;
        if (sscanf(p, "fan=%31[^|]|%lf", lab, &rpm) == 2) {
            size_t n = strlen(b);
            snprintf(b + n, sizeof b - n, "%s%s %.0f", n ? "   " : "", lab, rpm);
        }
    }
    double gf = num(&a, "gpu_fan");
    if (gf >= 0) {
        size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "%sgpu %.0f", n ? "   " : "", gf);
    }
    set_text(row_val[4], "%s rpm", b[0] ? b : "no sensors");

    temp(t1, sizeof t1, num(&a, "nvme_temp"));
    temp(t2, sizeof t2, num(&a, "board_temp"));
    temp(t3, sizeof t3, num(&a, "ccd2_temp"));
    char ccd1[32];
    temp(ccd1, sizeof ccd1, num(&a, "ccd1_temp"));
    set_text(row_val[5], "CCD1 %s   CCD2 %s   SSD %s   board %s", ccd1, t3, t1, t2);

    char nif[32] = "";
    proto_get(&a, "net_if", nif, sizeof nif);
    set_text(row_val[6], "%s %.0f Mb/s   down %.1f MB/s   up %.1f MB/s", nif, num(&a, "net_speed"),
             num(&a, "net_rx_kbs") / 1000, num(&a, "net_tx_kbs") / 1000);
}

static gboolean tick(gpointer u)
{
    (void)u;
    refresh_monitor();
    refresh_status();
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------ actions */

static gboolean apply_level(gpointer u)
{
    (void)u;
    apply_id = 0;
    char req[32];
    struct strbuf a;
    snprintf(req, sizeof req, "profile %d", (int)gtk_range_get_value(GTK_RANGE(scale)));
    call(req, &a);
    refresh_status();
    return G_SOURCE_REMOVE;
}

static void on_scale(GtkRange *r, gpointer u)
{
    (void)u;
    double v = round(gtk_range_get_value(r) / 10) * 10;
    if (fabs(v - gtk_range_get_value(r)) > 0.01) {
        gtk_range_set_value(r, v); /* snaps, calls us again */
        return;
    }
    show_summary((int)v);
    if (updating)
        return;
    /* apply when the slider rests for a moment, not on every step */
    if (apply_id)
        g_source_remove(apply_id);
    apply_id = g_timeout_add(350, apply_level, NULL);
}

static void on_net(GObject *o, GParamSpec *ps, gpointer u)
{
    (void)ps;
    (void)u;
    if (updating)
        return;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    if (i >= G_N_ELEMENTS(net_ids))
        return;
    char req[48];
    struct strbuf a;
    snprintf(req, sizeof req, "net %s", net_ids[i]);
    call(req, &a);
    refresh_status();
}

static gboolean on_auto(GtkSwitch *s, gboolean state, gpointer u)
{
    (void)s;
    (void)u;
    if (updating)
        return FALSE;
    struct strbuf a;
    call(state ? "auto on" : "auto off", &a);
    refresh_status();
    return FALSE;
}

static void on_stock(GtkButton *b, gpointer u)
{
    (void)b;
    (void)u;
    struct strbuf a;
    call("stock", &a);
    refresh_status();
}

static void load_caps(void)
{
    struct strbuf a;
    if (call("caps", &a))
        return;
    char *body_txt = strchr(a.s, '\n');
    gtk_label_set_text(GTK_LABEL(caps_label), body_txt ? body_txt + 1 : "");
}

/* ------------------------------------------------------------- layout */

static GtkWidget *heading(const char *t)
{
    GtkWidget *l = gtk_label_new(t);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *dim(const char *t)
{
    GtkWidget *l = gtk_label_new(t);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    return l;
}

static void activate(GtkApplication *app, gpointer u)
{
    (void)u;
    if (win) {
        gtk_window_present(GTK_WINDOW(win));
        return;
    }
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
                                      ".pct { font-size: 28px; font-weight: bold; }"
                                      ".val { font-family: monospace; }"
                                      ".section { margin-top: 14px; }"
                                      "levelbar { min-height: 4px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Gentoo Power Control");
    gtk_window_set_default_size(GTK_WINDOW(win), 600, -1);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(outer, 18);
    gtk_widget_set_margin_end(outer, 18);
    gtk_widget_set_margin_top(outer, 14);
    gtk_widget_set_margin_bottom(outer, 16);
    gtk_window_set_child(GTK_WINDOW(win), outer);

    conn_label = dim(" ");
    gtk_box_append(GTK_BOX(outer), conn_label);
    body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(outer), body);

    /* power level */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *h = heading("Power level");
    gtk_widget_set_hexpand(h, TRUE);
    gtk_widget_set_valign(h, GTK_ALIGN_CENTER);
    pct_label = gtk_label_new("–");
    gtk_widget_add_css_class(pct_label, "pct");
    gtk_box_append(GTK_BOX(top), h);
    gtk_box_append(GTK_BOX(top), pct_label);
    gtk_box_append(GTK_BOX(body), top);

    scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 50, 100, 10);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    for (int v = 50; v <= 100; v += 10) {
        char m[8];
        snprintf(m, sizeof m, "%d", v);
        gtk_scale_add_mark(GTK_SCALE(scale), v, GTK_POS_BOTTOM, m);
    }
    gtk_range_set_value(GTK_RANGE(scale), 80);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale), NULL);
    gtk_box_append(GTK_BOX(body), scale);
    summary = dim(" ");
    gtk_box_append(GTK_BOX(body), summary);

    /* network */
    GtkWidget *nrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(nrow, "section");
    h = heading("Network");
    gtk_widget_set_hexpand(h, TRUE);
    gtk_box_append(GTK_BOX(nrow), h);
    const char *net_names[] = {"Follow power level", "Balanced", "Low latency", "Throughput", NULL};
    net_drop = gtk_drop_down_new_from_strings(net_names);
    g_signal_connect(net_drop, "notify::selected", G_CALLBACK(on_net), NULL);
    gtk_box_append(GTK_BOX(nrow), net_drop);
    gtk_box_append(GTK_BOX(body), nrow);
    net_summary = dim(" ");
    gtk_box_append(GTK_BOX(body), net_summary);

    /* auto */
    GtkWidget *arow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(arow, "section");
    h = heading("Go to 100% for games and builds");
    gtk_widget_set_hexpand(h, TRUE);
    gtk_box_append(GTK_BOX(arow), h);
    auto_sw = gtk_switch_new();
    gtk_widget_set_valign(auto_sw, GTK_ALIGN_CENTER);
    g_signal_connect(auto_sw, "state-set", G_CALLBACK(on_auto), NULL);
    gtk_box_append(GTK_BOX(arow), auto_sw);
    gtk_box_append(GTK_BOX(body), arow);
    auto_note = dim(" ");
    gtk_box_append(GTK_BOX(body), auto_note);

    /* live readings */
    GtkWidget *mh = heading("Right now");
    gtk_widget_add_css_class(mh, "section");
    gtk_box_append(GTK_BOX(body), mh);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    const char *names[] = {"CPU", "GPU", "VRAM", "Memory", "Fans", "Temps", "Network"};
    int row = 0;
    for (int i = 0; i < 7; i++) {
        GtkWidget *n = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(n), 0);
        gtk_widget_add_css_class(n, "dim-label");
        row_val[i] = gtk_label_new("–");
        gtk_label_set_xalign(GTK_LABEL(row_val[i]), 0);
        gtk_widget_add_css_class(row_val[i], "val");
        gtk_widget_set_hexpand(row_val[i], TRUE);
        gtk_grid_attach(GTK_GRID(grid), n, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row_val[i], 1, row++, 1, 1);
        GtkWidget **bar = i == 0 ? &cpu_bar : i == 1 ? &gpu_bar : i == 3 ? &ram_bar : NULL;
        if (bar) {
            *bar = gtk_level_bar_new();
            gtk_level_bar_set_mode(GTK_LEVEL_BAR(*bar), GTK_LEVEL_BAR_MODE_CONTINUOUS);
            gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(*bar), GTK_LEVEL_BAR_OFFSET_LOW);
            gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(*bar), GTK_LEVEL_BAR_OFFSET_HIGH);
            gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(*bar), GTK_LEVEL_BAR_OFFSET_FULL);
            gtk_grid_attach(GTK_GRID(grid), *bar, 1, row++, 1, 1);
        }
    }
    gtk_box_append(GTK_BOX(body), grid);

    /* capabilities */
    GtkWidget *exp = gtk_expander_new("What this PC supports");
    gtk_widget_add_css_class(exp, "section");
    caps_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(caps_label), 0);
    gtk_label_set_selectable(GTK_LABEL(caps_label), TRUE);
    gtk_widget_add_css_class(caps_label, "val");
    gtk_expander_set_child(GTK_EXPANDER(exp), caps_label);
    gtk_box_append(GTK_BOX(body), exp);

    stock_btn = gtk_button_new_with_label("Undo everything (stock)");
    gtk_widget_set_halign(stock_btn, GTK_ALIGN_START);
    gtk_widget_add_css_class(stock_btn, "section");
    g_signal_connect(stock_btn, "clicked", G_CALLBACK(on_stock), NULL);
    gtk_box_append(GTK_BOX(body), stock_btn);

    load_caps();
    tick(NULL);
    g_timeout_add_seconds(1, tick, NULL);
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.optimzr.PowerControl", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
