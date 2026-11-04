# Otavix — FIFA Street 2 (Xbox): origem, licenças e referências

O port nativo do FIFA Street 2 recompila o `default.xbe` do disco do próprio jogador e roda sobre um runtime que imita o Xbox original. Este documento explica de onde vem cada parte e o que serviu de referência.

## 1. Base e componentes de terceiros (avisos exigidos por licença)

| Componente | Autor | Licença | Como entra |
|---|---|---|---|
| **Xbox XBE Recomp** (recompilador, runtime e harness, versão 0.1.0, em `sdk/`) | TGP482 e Joshhhuaaa ([TGP482/XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp)) | MIT | É a base do port, com as nossas mudanças por cima. O aviso original vai no pacote como `licencas/XboxXBERecomp.txt`. |
| toml++, Dear ImGui, SDL 3 | Os autores de cada projeto | MIT e zlib | Embutidos. Os avisos vão no `THIRD_PARTY.txt`. |
| **NVIDIA DLSS**, **Intel XeSS** | NVIDIA, Intel | Licenças dos fabricantes, que permitem redistribuir os binários | DLLs ao lado do executável. Os textos vão em `licencas/`. |
| **AMD FidelityFX** (FSR) | Advanced Micro Devices | MIT | DLLs ao lado do executável. O aviso vai em `licencas/`. |
| **LLVM/Clang** (o compilador que traduz o jogo no primeiro uso) | Projeto LLVM | Apache 2.0 com exceção LLVM | Em `compiler/`, com o `LLVM-LICENSE.txt`. |

## 2. Referências de comportamento (sem código copiado)

- **xemu** e **Cxbx-Reloaded**: o comportamento do hardware e das APIs do Xbox. O Xbox XBE Recomp não usa código de emulador, e o port também não.
- **xboxdevwiki**: os formatos e o comportamento do console.

## 3. O que é autoral (Otavix)

- As mudanças no runtime (`sdk/runtime`): a GPU em Direct3D 12 com os alvos de cor de 16 bits lidos de volta (report 513), os upscalers (DLSS, FSR, XeSS) e o meio quadro interpolado dos 120 fps, a entrada, o áudio, a leitura do disco e as mensagens de região e de arquivo errado.
- Os ganchos do jogo (`src/game`), como a guarda do jogador sem bloco de aparência (reports 550, 551 e 684).
- O empacotamento e a integração com o Otavix Launcher.
