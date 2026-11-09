# Este fork (ramonnteixeira/Hookshot-linux)

A branch `linux-port` é o projeto oficial ([NotRexed/Hookshot](https://github.com/NotRexed/Hookshot), `main`) mais
alguns arquivos próprios. Desde novembro de 2026 o oficial tem suporte a Linux; o port que este fork fez antes disso
(camada Win32 própria, janela SDL, fonte 8x8 no menu) está guardado na branch `linux-port-v1` e não é mais usado.

## O que o fork acrescenta

| Arquivo | Para quê |
|---|---|
| `tests/setjmp_test.c` | Testa o `setjmp`/`longjmp` escrito em assembly no Linux (o primeiro crash do port v1 foi aí) |
| `scripts/sync_upstream.sh` | Traz o oficial para a `linux-port`, roda o teste e envia |
| `.github/workflows/fork-checks.yml` | Roda o teste a cada push na `linux-port` |
| `docs/fork.md` | Esta nota |

O fork quase só **acrescenta** arquivos: assim cada sincronização é um merge sem conflitos. Um ajuste num arquivo do
oficial deve ir, de preferência, como pull request para lá. Por enquanto há um:

| Arquivo do oficial | Ajuste |
|---|---|
| `gpu/vk/vk_draw.cpp` (`fallback_compute_pipelines`) | Na MX250 (Pascal, driver NVIDIA) o compilador do driver quebra no substituto em compute do mesh `a5da9f84b72d1471` (corte para a Árvore Deku). O jogo sai do driver, mas a compilação seguinte desse programa nunca voltava: a tela ficava preta. Agora um programa que falhou não é compilado de novo, e o que quebrou o driver fica anotado em `hookshot_bad_fallback.txt`, ao lado do jogo, e nas próximas partidas é deixado de fora sem compilar (só esse modelo some). Apagar o arquivo tenta de novo (driver novo); `RX_VK_BAD_FALLBACK_KEEP=1` desliga a anotação |

Se o merge com o oficial der conflito nesse arquivo, fique com a versão do oficial e reaplique o trecho (ou descarte-o,
se o oficial já tiver resolvido).

## Atualizar com o oficial

```
cd ~/projects/Hookshot-linux
scripts/sync_upstream.sh
```

Depois, compile o jogo de novo com as mesmas opções de antes (os saves e o `hookshot_options.ini` ficam).

## Compilar no Linux

Como no `readme.md` do oficial ("On Linux"): pelo AppImage, ou pelo código com
`python3 -I nx2/make_game.py --nup <jogo.nup> --out <pasta> --language pt-BR`, com `RX_CPM` apontando para uma pasta
feita por `scripts/linux/deps.sh`.

## Memória do jogo e desempenho

O oficial escolhe a memória do jogo pensando nas placas AMD (RADV): sem huge pages, e um memfd compartilhado quando
`/dev/udmabuf` abre. Numa NVIDIA, dá para testar o contrário em `hookshot_env.txt`, ao lado do jogo:

```
HOS_THP=1
HOS_GUEST_MEMFD=0
```

`HOS_THP=1` só muda algo se o kernel estiver com huge pages em `always`
(`cat /sys/kernel/mm/transparent_hugepage/enabled`); no modo `madvise`, padrão do Ubuntu, a memória do jogo não as usa de
qualquer jeito. O log diz qual memória foi usada (linhas `mem:`), e o fps das duas formas se compara pelo contador do menu Insert.
