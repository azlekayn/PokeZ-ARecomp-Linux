// The save sync's panel (runtime/saveserv.cpp), drawn into the current window: the start screen's Saves page, and the F1
// menu's Saves tab (in game: uploads only, so a conflict is explained there and settled on the start screen).
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <string>

#include "imgui.h"
#include "osk.h"
#include "saveserv.h"
extern "C" {
#include "settings.h"
}
#define TR(en, pt) UI_TR(en, pt)

static std::string ago(long long t) {
    long long d = (long long)time(NULL) - t;
    char b[64];
    if (d < 60) return TR("just now", "agora mesmo");
    if (d < 3600) snprintf(b, sizeof b, TR("%lld min ago", "há %lld min"), d / 60);
    else if (d < 86400) snprintf(b, sizeof b, TR("%lld h ago", "há %lld h"), d / 3600);
    else snprintf(b, sizeof b, TR("%lld d ago", "há %lld d"), d / 86400);
    return b;
}

extern "C" void saveserv_panel_draw(float s) {
    SaveServStatus st = saveserv_status();
    static char server[256];
    static bool filled;
    if (!filled) {
        filled = true;
        snprintf(server, sizeof server, "%s", saveserv_server_hint().c_str());
    }
    const ImVec2 btn(0, 40 * s);
    ImGui::PushTextWrapPos(0);
    bool paired = !st.account.empty();

    if (st.state == SAVESERV_PAIRING) {
        ImGui::TextUnformatted(TR("On the SaveServ website, open the + cover and type this PIN:",
                                  "No site do SaveServ, abra a capa + e digite este PIN:"));
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.27f, 1), "%s", st.pin.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", st.server.c_str());
        if (ImGui::Button(TR("Cancel", "Cancelar"), btn)) saveserv_pair_cancel();
    } else if (!paired) {
        ImGui::TextDisabled("%s", st.unpaired_remotely
                                      ? TR("This device was removed on the website. Pair it again to keep syncing.",
                                           "Este aparelho foi removido no site. Pareie de novo para continuar sincronizando.")
                                      : TR("Keeps your save the same across your devices, through your own SaveServ server.",
                                           "Mantém o seu save igual em todos os aparelhos, pelo seu próprio servidor SaveServ."));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ui_input_text("##saveserv_server", server, sizeof server, 0, TR("Server address", "Endereço do servidor"),
                      TR("Server address", "Endereço do servidor"));
        ImGui::BeginDisabled(!server[0] || st.state == SAVESERV_BUSY);
        if (ImGui::Button(st.state == SAVESERV_BUSY ? TR("Contacting...", "Conectando...") : TR("Pair this device", "Parear este aparelho"), btn))
            saveserv_pair_start(server);
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("%s · %s", st.account.c_str(), st.server.c_str());
        std::string from = st.remote_device.empty() ? TR("another device", "outro aparelho") : st.remote_device;
        switch (st.state) {
        case SAVESERV_BUSY: ImGui::TextUnformatted(TR("Syncing...", "Sincronizando...")); break;
        case SAVESERV_SYNCED:
            if (!st.when) ImGui::TextUnformatted(TR("Not synced yet.", "Ainda não sincronizado."));
            else {
                const char *what = st.last_action == SAVESERV_UPLOADED     ? TR("Uploaded", "Enviado")
                                   : st.last_action == SAVESERV_DOWNLOADED ? TR("Downloaded", "Baixado")
                                                                           : TR("Up to date", "Em dia");
                ImGui::TextColored(ImVec4(0.29f, 0.82f, 0.54f, 1), "%s · %s %s", what, TR("checked", "verificado"), ago(st.when).c_str());
            }
            break;
        case SAVESERV_OFFLINE:
            ImGui::TextColored(ImVec4(1, 0.54f, 0.44f, 1), "%s", TR("Server not reachable: your local save is used, and synced later.",
                                                                     "Servidor fora do ar: o save local é usado, e sincronizado depois."));
            break;
        case SAVESERV_NEWER:
            ImGui::TextColored(ImVec4(1, 0.54f, 0.44f, 1), TR("%s saved a newer game (%s). Nothing is sent while you play: restart from the start screen to get it.",
                                                              "%s salvou um jogo mais novo (%s). Nada é enviado enquanto você joga: volte à tela inicial para pegar."),
                               from.c_str(), ago(st.remote_updated).c_str());
            break;
        case SAVESERV_CONFLICT:
            ImGui::TextColored(ImVec4(1, 0.54f, 0.44f, 1), TR("This device and %s (%s) both saved since the last sync. Which one do you keep?",
                                                              "Este aparelho e %s (%s) salvaram desde a última sincronização. Qual você mantém?"),
                               from.c_str(), ago(st.remote_updated).c_str());
            if (st.in_game) {
                ImGui::TextDisabled("%s", TR("Choose on the start screen.", "Escolha na tela inicial."));
            } else {
                ImGui::TextDisabled("%s", TR("The other copy isn't lost: the server keeps every upload, and this device's goes to Backups.",
                                             "A outra cópia não se perde: o servidor guarda cada envio, e a deste aparelho vai para os Backups."));
                if (ImGui::Button(TR("Keep this device's", "Manter a deste aparelho"), btn)) saveserv_resolve(true);
                ImGui::SameLine();
                if (ImGui::Button(TR("Use the server's", "Usar a do servidor"), btn)) saveserv_resolve(false);
            }
            break;
        default: ImGui::TextColored(ImVec4(1, 0.54f, 0.44f, 1), "%s", st.message.c_str()); break;
        }
        if (st.state != SAVESERV_CONFLICT) {
            ImGui::BeginDisabled(st.state == SAVESERV_BUSY);
            if (ImGui::Button(TR("Sync now", "Sincronizar"), btn)) saveserv_kick();
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        static bool ask;
        if (st.state == SAVESERV_CONFLICT) {
            // (one question at a time)
        } else if (!ask) {
            if (ImGui::Button(TR("Unpair", "Desparear"), btn)) ask = true;
        } else {
            if (ImGui::Button(TR("Yes, unpair", "Sim, desparear"), btn)) saveserv_unpair(), ask = false;
            ImGui::SameLine();
            if (ImGui::Button(TR("Cancel", "Cancelar"), btn)) ask = false;
        }
    }
    if (!paired && st.state == SAVESERV_ERROR) ImGui::TextColored(ImVec4(1, 0.54f, 0.44f, 1), "%s", st.message.c_str());
    if (!paired && st.state == SAVESERV_OFFLINE)
        ImGui::TextColored(ImVec4(1, 0.54f, 0.44f, 1), "%s", TR("That server didn't answer.", "Esse servidor não respondeu."));
    ImGui::PopTextWrapPos();
}
