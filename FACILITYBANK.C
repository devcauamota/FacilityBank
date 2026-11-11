
==============================================================================
=== ARQUIVO: src/banco.h
==============================================================================
/* banco.h - "Banco de dados" simulado em memoria do Facility Bank.
 * Tudo aqui e ficticio. Em producao, estas funcoes viram chamadas ao core bancario. */
#ifndef BANCO_H
#define BANCO_H

#include <stddef.h>

typedef long long centavos;          /* dinheiro SEMPRE em centavos (nunca float) */

#define MAX_EXTRATO 50
#define MAX_BENEF   10
#define MAX_BOLETOS 8
#define MAX_CONTAS  32               /* conta de demonstracao + cadastros (e contas lidas do banco) */

typedef struct {
    char     quando[16];             /* "18/10 09:30" */
    char     descricao[56];
    centavos valor;                  /* negativo = saida */
} Lancamento;

typedef struct {
    char apelido[24];
    char nome[40];
    char chave[48];
} Beneficiario;

typedef struct {
    char     codigo[49];             /* linha digitavel, so digitos */
    char     cedente[40];
    centavos valor;
    char     vencimento[8];          /* "19/10" */
    int      pago;
} Boleto;

typedef struct {
    char chave[48];
    char nome[40];
    int  conta_recente;              /* 1 = conta aberta ha menos de 30 dias (risco) */
} ContaPix;

typedef struct {
    char nome[40];
    char cpf[12];
    char nasc[12];                   /* "25/03/1950" */
    char tel[12];                    /* celular com DDD, so digitos */
    char numero[12];                 /* numero da conta: "004217-5" */
    char abertura[12];               /* data de abertura: "18/10/2026" */
    char pin[5];                     /* DEMO: em producao nunca guardar senha em texto */
    int  persistida;                 /* 1 = cadastro esta no SQL Server (senha so la, em hash; pin vazio) */
    centavos saldo;

    Lancamento extrato[MAX_EXTRATO];
    int        n_extrato;

    Beneficiario benef[MAX_BENEF];
    int          n_benef;

    Boleto boletos[MAX_BOLETOS];
    int    n_boletos;

    char     cartao_final[5];
    centavos cartao_limite;
    centavos cartao_fatura;
    char     cartao_venc[8];
    int      cartao_bloqueado;

    int pix_bloqueado;

    int      acomp_ativo;
    char     acomp_nome[40];
    char     acomp_tel[16];
    centavos acomp_limite;           /* a partir deste valor o acompanhante aprova */
} Conta;

void  banco_init(void);
Conta *banco_conta_por_cpf(const char *cpf11);
Conta *banco_conta_demo(void);
/* 1 = a ULTIMA operacao de cadastro/login falhou porque o banco de dados esta fora do ar
 * (e nao porque o CPF/senha esta errado). Sempre 0 quando nao ha banco configurado. */
int  banco_indisponivel(void);
/* 1 = senha confere | 0 = nao confere | -1 = banco indisponivel */
int  banco_pin_confere(const Conta *c, const char *pin4);
/* Abre uma conta nova (saldo zero, sem cartao). NULL = nao ha mais espaco.
 * O chamador ja deve ter conferido que o CPF nao esta cadastrado. */
Conta *banco_conta_criar(const char *nome, const char *cpf11, const char *nasc,
                         const char *tel, const char *pin);

int  cpf_valido(const char *cpf11);
/* Data de nascimento "dd/mm/aaaa" (aceita tambem so digitos ou - . como separador).
 * 1 = ok (saida normalizada "dd/mm/aaaa") | 0 = invalida | -1 = menor de 18 anos */
int  parse_data_nasc(const char *texto, char *saida, size_t n);
int  parse_valor(const char *texto, centavos *saida);     /* 1 = ok */
void fmt_valor(centavos v, char *buf, size_t n);          /* "R$ 1.234,56" */

int  banco_pix_lookup(const char *chave, ContaPix *out);  /* 1 = encontrou */
int  banco_benef_por_chave(const Conta *c, const char *chave); /* indice ou -1 */
int  banco_benef_add(Conta *c, const char *apelido, const char *nome, const char *chave);
void banco_benef_remove(Conta *c, int idx);
int  banco_boleto_por_codigo(const Conta *c, const char *digitos); /* indice ou -1 */
void banco_lancar(Conta *c, const char *descricao, centavos valor); /* ajusta saldo e extrato */

#endif

==============================================================================
=== ARQUIVO: src/banco.c
==============================================================================
/* banco.c - dados ficticios e regras basicas de dinheiro/CPF */
#include "banco.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USAR_SQL
#include "db.h"
#define COM_BANCO() db_configurado()
#else
#define COM_BANCO() 0
#endif

static Conta g_contas[MAX_CONTAS];   /* [0] = conta de demonstracao; as demais sao cadastros/leituras do banco */
static int   g_n_contas;
static int   g_falha_db;             /* ultima operacao falhou por banco fora do ar */

static const ContaPix g_diretorio[] = {
    { "ana.silva@email.com",                    "Ana Beatriz Silva",      0 },
    { "11144477735",                            "Joao Pedro Alves",       0 },
    { "11987654321",                            "Padaria Bom Pao LTDA",   0 },
    { "carlos.m@email.com",                     "Carlos Mendes",          0 },
    { "9f8e7d6c-5b4a-4321-8fed-cba987654321",   "Clinica Saude Total",    0 },
    { "novo.contato@mail-premio.com",           "Premios Rapidos ME",     1 },
};

static void agora(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(buf, n, "%d/%m %H:%M", tm);
    else    snprintf(buf, n, "--/-- --:--");
}

static void hoje(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(buf, n, "%d/%m/%Y", tm);
    else    snprintf(buf, n, "--/--/----");
}

static void copia(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

static void semear_boleto(Conta *c, int seed, const char *cedente, centavos v, const char *venc)
{
    Boleto *b = &c->boletos[c->n_boletos++];
    int i;
    for (i = 0; i < 47; i++) b->codigo[i] = (char)('0' + ((seed * 7 + i * 3 + i / 5) % 10));
    b->codigo[47] = '\0';
    copia(b->cedente, sizeof b->cedente, cedente);
    b->valor = v;
    copia(b->vencimento, sizeof b->vencimento, venc);
    b->pago = 0;
}

static void semear_lanc(Conta *c, const char *quando, const char *desc, centavos v)
{
    Lancamento *l = &c->extrato[c->n_extrato++];
    copia(l->quando, sizeof l->quando, quando);
    copia(l->descricao, sizeof l->descricao, desc);
    l->valor = v;
}

void banco_init(void)
{
    Conta *c = &g_contas[0];
    memset(g_contas, 0, sizeof g_contas);
    g_n_contas = 1;

    copia(c->nome, sizeof c->nome, "Maria Aparecida Silva");
    copia(c->cpf, sizeof c->cpf, "52998224725");
    copia(c->nasc, sizeof c->nasc, "12/03/1951");
    copia(c->tel, sizeof c->tel, "11991234567");
    copia(c->numero, sizeof c->numero, "004217-5");
    copia(c->abertura, sizeof c->abertura, "03/02/2019");
    copia(c->pin, sizeof c->pin, "1234");
    c->saldo = 235000;                           /* R$ 2.350,00 */

    semear_lanc(c, "10/10 08:00", "Beneficio INSS",             185000);
    semear_lanc(c, "11/10 10:15", "Farmacia Saude",              -8790);
    semear_lanc(c, "12/10 17:40", "Mercado Bom Preco",          -23710);
    semear_lanc(c, "14/10 09:02", "Pix recebido - Ana Beatriz",  20000);
    semear_lanc(c, "15/10 14:30", "Conta de agua",               -8990);
    semear_lanc(c, "16/10 11:11", "Pix enviado - Joao Pedro",   -15000);

    banco_benef_add(c, "Filha Ana",       "Ana Beatriz Silva",    "ana.silva@email.com");
    banco_benef_add(c, "Joao Pedro",      "Joao Pedro Alves",     "11144477735");
    banco_benef_add(c, "Padaria",         "Padaria Bom Pao LTDA", "11987654321");

    semear_boleto(c, 1, "CEMIG Energia",          18745, "19/10");
    semear_boleto(c, 2, "Condominio Res. Flores", 54000, "28/10");
    semear_boleto(c, 3, "Fatura Cartao Facility Bank", 61230, "25/10");

    copia(c->cartao_final, sizeof c->cartao_final, "4321");
    c->cartao_limite = 300000;
    c->cartao_fatura = 61230;
    copia(c->cartao_venc, sizeof c->cartao_venc, "25/10");
}

Conta *banco_conta_demo(void) { return &g_contas[0]; }

int banco_indisponivel(void) { return g_falha_db; }

/* Memoria primeiro (inclui a conta de demonstracao); se nao achar e houver banco, busca nele. */
Conta *banco_conta_por_cpf(const char *cpf11)
{
    int i;
    g_falha_db = 0;
    for (i = 0; i < g_n_contas; i++)
        if (strcmp(cpf11, g_contas[i].cpf) == 0) return &g_contas[i];
#ifdef USAR_SQL
    if (COM_BANCO() && g_n_contas < MAX_CONTAS) {
        int r = db_conta_por_cpf(cpf11, &g_contas[g_n_contas]);
        if (r < 0) { g_falha_db = 1; return NULL; }
        if (r > 0) return &g_contas[g_n_contas++];
    } else if (COM_BANCO()) {
        g_falha_db = 1;                          /* sem espaco para mais contas em memoria */
    }
#endif
    return NULL;
}

int banco_pin_confere(const Conta *c, const char *pin4)
{
    g_falha_db = 0;
    if (!c->persistida) return strcmp(pin4, c->pin) == 0;
#ifdef USAR_SQL
    {
        int r = db_pin_confere(c->cpf, pin4);
        if (r < 0) g_falha_db = 1;
        return r;
    }
#else
    return 0;
#endif
}

Conta *banco_conta_criar(const char *nome, const char *cpf11, const char *nasc,
                         const char *tel, const char *pin)
{
    Conta *c;
    unsigned long long seq;
    int dv = 0, i;
    char digs[8];

    g_falha_db = 0;
    if (g_n_contas >= MAX_CONTAS) return NULL;
    c = &g_contas[g_n_contas];
    memset(c, 0, sizeof *c);

    copia(c->nome, sizeof c->nome, nome);
    copia(c->cpf, sizeof c->cpf, cpf11);
    copia(c->nasc, sizeof c->nasc, nasc);
    copia(c->tel, sizeof c->tel, tel);
    copia(c->pin, sizeof c->pin, pin);
    hoje(c->abertura, sizeof c->abertura);

    seq = 100000u + (unsigned)g_n_contas * 4217u % 900000u;   /* numero ficticio, sequencial */
#ifdef USAR_SQL
    if (COM_BANCO() && !db_proxima_sequencia(&seq)) {          /* com banco, o numero vem da sequencia dele */
        g_falha_db = 1;
        memset(c, 0, sizeof *c);
        return NULL;
    }
#endif
    snprintf(digs, sizeof digs, "%06u", (unsigned)(seq % 1000000u));
    for (i = 0; digs[i]; i++) dv += digs[i] - '0';
    snprintf(c->numero, sizeof c->numero, "%s-%d", digs, dv % 10);

#ifdef USAR_SQL
    if (COM_BANCO()) {
        if (!db_conta_criar(c, pin)) {                         /* nada foi gravado */
            g_falha_db = 1;
            memset(c, 0, sizeof *c);
            return NULL;
        }
        c->persistida = 1;
        memset(c->pin, 0, sizeof c->pin);                      /* a senha so existe no banco, em hash */
    }
#endif
    g_n_contas++;
    return c;
}

int parse_data_nasc(const char *s, char *out, size_t n)
{
    static const int dias[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    char d[9];
    int len = 0, dia, mes, ano, max, idade;
    time_t t;
    struct tm *tm;

    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            if (len >= 8) return 0;
            d[len++] = *s;
        } else if (*s != '/' && *s != '-' && *s != '.' && *s != ' ') {
            return 0;
        }
    }
    if (len != 8) return 0;
    d[8] = '\0';

    dia = (d[0] - '0') * 10 + (d[1] - '0');
    mes = (d[2] - '0') * 10 + (d[3] - '0');
    ano = (d[4] - '0') * 1000 + (d[5] - '0') * 100 + (d[6] - '0') * 10 + (d[7] - '0');
    if (mes < 1 || mes > 12 || ano < 1900) return 0;
    max = dias[mes - 1];
    if (mes == 2 && ((ano % 4 == 0 && ano % 100 != 0) || ano % 400 == 0)) max = 29;
    if (dia < 1 || dia > max) return 0;

    t = time(NULL);
    tm = localtime(&t);
    if (tm) {
        idade = (tm->tm_year + 1900) - ano;
        if (tm->tm_mon + 1 < mes || (tm->tm_mon + 1 == mes && tm->tm_mday < dia)) idade--;
        if (idade < 0 || idade > 120) return 0;
        if (idade < 18) return -1;
    }
    snprintf(out, n, "%02d/%02d/%04d", dia, mes, ano);
    return 1;
}

int cpf_valido(const char *c)
{
    int i, soma, d1, d2, iguais = 1;
    if (strlen(c) != 11) return 0;
    for (i = 0; i < 11; i++) {
        if (!isdigit((unsigned char)c[i])) return 0;
        if (c[i] != c[0]) iguais = 0;
    }
    if (iguais) return 0;
    soma = 0;
    for (i = 0; i < 9; i++) soma += (c[i] - '0') * (10 - i);
    d1 = (soma * 10) % 11; if (d1 == 10) d1 = 0;
    soma = 0;
    for (i = 0; i < 10; i++) soma += (c[i] - '0') * (11 - i);
    d2 = (soma * 10) % 11; if (d2 == 10) d2 = 0;
    return d1 == c[9] - '0' && d2 == c[10] - '0';
}

/* Aceita: 150 | 150,50 | 150.50 | 1.500 | 1.500,00 | R$ 1500 */
int parse_valor(const char *s, centavos *out)
{
    char buf[32];
    size_t j = 0, k, ult = (size_t)-1;
    long long reais = 0;
    int cents = 0, dec, ndig = 0;

    for (; *s; s++) {
        if (isdigit((unsigned char)*s) || *s == ',' || *s == '.') {
            if (j >= sizeof buf - 1) return 0;
            buf[j++] = *s;
        } else if (*s == ' ' || *s == '$' || *s == 'R' || *s == 'r') {
            continue;
        } else {
            return 0;
        }
    }
    buf[j] = '\0';
    if (j == 0) return 0;

    for (k = 0; k < j; k++)
        if (buf[k] == ',' || buf[k] == '.') ult = k;

    if (ult == (size_t)-1) {
        dec = -1;                                  /* so inteiro */
    } else {
        dec = (int)(j - ult - 1);
        if (buf[ult] == ',' && (dec < 1 || dec > 2)) return 0;
        if (buf[ult] == '.' && dec != 1 && dec != 2 && dec != 3) return 0;
    }

    /* parte inteira: ignora separadores de milhar */
    {
        size_t fim = (dec == -1 || dec == 3) ? j : ult;
        for (k = 0; k < fim; k++) {
            if (buf[k] == ',' || buf[k] == '.') continue;
            reais = reais * 10 + (buf[k] - '0');
            if (++ndig > 9) return 0;
        }
    }
    if (dec == 1 || dec == 2) {
        cents = (buf[ult + 1] - '0') * (dec == 1 ? 10 : 1);
        if (dec == 2) cents = (buf[ult + 1] - '0') * 10 + (buf[ult + 2] - '0');
    }
    if (ndig == 0) return 0;
    *out = reais * 100 + cents;
    return *out > 0;
}

void fmt_valor(centavos v, char *buf, size_t n)
{
    int neg = v < 0, len, i, k = 0;
    char t[24], o[32];
    if (neg) v = -v;
    snprintf(t, sizeof t, "%lld", v / 100);
    len = (int)strlen(t);
    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) o[k++] = '.';
        o[k++] = t[i];
    }
    o[k] = '\0';
    snprintf(buf, n, "%sR$ %s,%02d", neg ? "-" : "", o, (int)(v % 100));
}

int banco_pix_lookup(const char *chave, ContaPix *out)
{
    size_t i;
    for (i = 0; i < sizeof g_diretorio / sizeof g_diretorio[0]; i++) {
        if (strcmp(g_diretorio[i].chave, chave) == 0) {
            *out = g_diretorio[i];
            return 1;
        }
    }
    return 0;
}

int banco_benef_por_chave(const Conta *c, const char *chave)
{
    int i;
    for (i = 0; i < c->n_benef; i++)
        if (strcmp(c->benef[i].chave, chave) == 0) return i;
    return -1;
}

int banco_benef_add(Conta *c, const char *apelido, const char *nome, const char *chave)
{
    Beneficiario *b;
    if (c->n_benef >= MAX_BENEF) return -1;
    if (banco_benef_por_chave(c, chave) >= 0) return -2;
    b = &c->benef[c->n_benef++];
    copia(b->apelido, sizeof b->apelido, apelido);
    copia(b->nome, sizeof b->nome, nome);
    copia(b->chave, sizeof b->chave, chave);
    return 0;
}

void banco_benef_remove(Conta *c, int idx)
{
    if (idx < 0 || idx >= c->n_benef) return;
    memmove(&c->benef[idx], &c->benef[idx + 1],
            (size_t)(c->n_benef - idx - 1) * sizeof c->benef[0]);
    c->n_benef--;
}

int banco_boleto_por_codigo(const Conta *c, const char *digitos)
{
    int i;
    for (i = 0; i < c->n_boletos; i++)
        if (strcmp(c->boletos[i].codigo, digitos) == 0) return i;
    return -1;
}

void banco_lancar(Conta *c, const char *descricao, centavos valor)
{
    Lancamento *l;
    if (c->n_extrato >= MAX_EXTRATO) {
        memmove(&c->extrato[0], &c->extrato[1], (MAX_EXTRATO - 1) * sizeof c->extrato[0]);
        c->n_extrato--;
    }
    l = &c->extrato[c->n_extrato++];
    agora(l->quando, sizeof l->quando);
    copia(l->descricao, sizeof l->descricao, descricao);
    l->valor = valor;
    c->saldo += valor;
}

==============================================================================
=== ARQUIVO: src/db.h
==============================================================================
/* db.h - camada SQL Server (ODBC) do Facility Bank. So existe no servidor web (Windows),
 * compilado com -DUSAR_SQL. Sem banco configurado, o programa segue usando a memoria.
 *
 * Quem chama e banco.c; o bot (bot.c) nunca fala direto com o banco. */
#ifndef DB_H
#define DB_H

#include "banco.h"

/* Le a conexao (variavel FB_DB_CONN ou arquivo banco.conf) e tenta conectar.
 * 1 = conectado | 0 = nao configurado ou falhou (veja db_configurado). */
int db_iniciar(void);

/* 1 = ha uma conexao configurada. Nesse caso o banco e a fonte da verdade: se ele cair,
 * as operacoes FALHAM (nunca voltam em silencio para a memoria). */
int db_configurado(void);

/* 1 = achou e preencheu *c | 0 = CPF sem cadastro | -1 = banco indisponivel */
int db_conta_por_cpf(const char *cpf11, Conta *c);

/* Proximo numero da sequencia de contas. 1 = ok */
int db_proxima_sequencia(unsigned long long *seq);

/* Grava cliente + conta numa transacao (senha vira sal + SHA-256 no proprio SQL Server).
 * 1 = ok | 0 = falhou (nada foi gravado) */
int db_conta_criar(const Conta *c, const char *pin4);

/* 1 = senha confere | 0 = nao confere | -1 = banco indisponivel */
int db_pin_confere(const char *cpf11, const char *pin4);

#endif

==============================================================================
=== ARQUIVO: src/db.c
==============================================================================
/* db.c - SQL Server via ODBC (Windows). Compile com: -DUSAR_SQL src/db.c -lodbc32
 *
 * Conexao: variavel de ambiente FB_DB_CONN ou a primeira linha util de banco.conf
 * (nunca no codigo, nunca no log). Exemplos:
 *   Driver={ODBC Driver 17 for SQL Server};Server=NOME-DO-PC\SQLEXPRESS;Database=FaciliteBank;UID=chatbot_facilite;PWD=...;
 *   Driver={SQL Server};Server=192.168.0.10,1433;Database=FaciliteBank;Trusted_Connection=Yes;
 *
 * Toda consulta usa parametros (?), nunca texto montado com dados do cliente. */
#include "db.h"

#include <windows.h>
#include <sql.h>
#include <sqlext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEMPO_LOGIN_S   5
#define TEMPO_CONSULTA_S 10

static SQLHENV g_env;
static SQLHDBC g_dbc;
static int     g_conectado;
static int     g_configurado;
static char    g_conn[1024];

/* ------------------------------------------------------------------ util */

static void diag(SQLSMALLINT tipo, SQLHANDLE h, const char *onde)
{
    SQLCHAR estado[8] = "", msg[512] = "";
    SQLINTEGER nativo = 0;
    SQLSMALLINT n = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRec(tipo, h, 1, estado, &nativo, msg, sizeof msg, &n)))
        fprintf(stderr, "[banco] %s: %s (SQLSTATE %s)\n", onde, (char *)msg, (char *)estado);
    else
        fprintf(stderr, "[banco] %s: falhou, sem detalhe\n", onde);
    if (strncmp((char *)estado, "08", 2) == 0 || strcmp((char *)estado, "HYT00") == 0)
        g_conectado = 0;                       /* conexao caiu: a proxima operacao reconecta */
}

static int u8_para_w(const char *src, SQLWCHAR *dst, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, src, -1, (wchar_t *)dst, cap) > 0;
}

static void w_para_u8(const SQLWCHAR *src, char *dst, int cap)
{
    if (WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)src, -1, dst, cap, NULL, NULL) <= 0)
        dst[0] = '\0';
}

/* "25/03/1950" -> "1950-03-25" (formato ISO: o SQL Server entende sem depender do idioma) */
static void data_iso(const char *br, char *out)
{
    if (strlen(br) == 10 && br[2] == '/' && br[5] == '/')
        snprintf(out, 11, "%.4s-%.2s-%.2s", br + 6, br + 3, br);
    else
        snprintf(out, 11, "1900-01-01");
}

static int le_conexao(void)
{
    static const char *arquivos[] = { "banco.conf", "../banco.conf", "chatbot-bancario/banco.conf" };
    const char *env = getenv("FB_DB_CONN");
    size_t i;
    if (env && *env) { snprintf(g_conn, sizeof g_conn, "%s", env); return 1; }
    for (i = 0; i < sizeof arquivos / sizeof arquivos[0]; i++) {
        FILE *f = fopen(arquivos[i], "r");
        char linha[1024];
        if (!f) continue;
        while (fgets(linha, sizeof linha, f)) {
            size_t l = strlen(linha);
            while (l > 0 && (linha[l - 1] == '\n' || linha[l - 1] == '\r' || linha[l - 1] == ' ')) linha[--l] = '\0';
            if (l == 0 || linha[0] == '#') continue;
            snprintf(g_conn, sizeof g_conn, "%s", linha);
            fclose(f);
            return 1;
        }
        fclose(f);
    }
    return 0;
}

/* ------------------------------------------------------------- conexao */

int db_configurado(void) { return g_configurado; }

int db_iniciar(void)
{
    SQLRETURN r;
    SQLCHAR saida[512];
    SQLSMALLINT n = 0;

    if (g_conectado) return 1;
    if (!g_configurado) {
        if (!le_conexao()) {
            printf("[banco] nenhum banco configurado (banco.conf ou FB_DB_CONN): usando dados em memoria.\n");
            return 0;
        }
        g_configurado = 1;
    }
    if (!g_env) {
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_env))) { g_env = NULL; return 0; }
        SQLSetEnvAttr(g_env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    }
    if (g_dbc) { SQLFreeHandle(SQL_HANDLE_DBC, g_dbc); g_dbc = NULL; }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, g_env, &g_dbc))) { g_dbc = NULL; return 0; }
    SQLSetConnectAttr(g_dbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)(SQLULEN)TEMPO_LOGIN_S, 0);

    r = SQLDriverConnect(g_dbc, NULL, (SQLCHAR *)g_conn, SQL_NTS, saida, sizeof saida, &n, SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(r)) {
        diag(SQL_HANDLE_DBC, g_dbc, "nao consegui conectar ao SQL Server");
        SQLFreeHandle(SQL_HANDLE_DBC, g_dbc);
        g_dbc = NULL;
        g_conectado = 0;
        return 0;
    }
    g_conectado = 1;
    printf("[banco] conectado ao SQL Server.\n");
    return 1;
}

static SQLHSTMT novo_stmt(void)
{
    SQLHSTMT st = NULL;
    if (!g_configurado) return NULL;
    if (!g_conectado && !db_iniciar()) return NULL;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st))) { diag(SQL_HANDLE_DBC, g_dbc, "alocar comando"); return NULL; }
    SQLSetStmtAttr(st, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)(SQLULEN)TEMPO_CONSULTA_S, 0);
    return st;
}

static void libera(SQLHSTMT st) { if (st) SQLFreeHandle(SQL_HANDLE_STMT, st); }

static int falhou(SQLRETURN r, SQLHSTMT st, const char *onde)
{
    if (SQL_SUCCEEDED(r)) return 0;
    diag(SQL_HANDLE_STMT, st, onde);
    return 1;
}

/* ------------------------------------------------------------- operacoes */

int db_conta_por_cpf(const char *cpf11, Conta *c)
{
    static const char sql[] =
        "SELECT cl.nome, CONVERT(CHAR(10), cl.data_nascimento, 103), cl.telefone, ct.numero, "
        "       CAST(ct.saldo * 100 AS BIGINT), CONVERT(CHAR(10), ct.data_abertura, 103) "
        "FROM dbo.CLIENTE cl JOIN dbo.CONTA ct ON ct.id_cliente = cl.id_cliente "
        "WHERE cl.cpf = ?";
    SQLHSTMT st = novo_stmt();
    SQLWCHAR nome[101];
    char nasc[12], tel[12], numero[12], abertura[12];     /* mesmos tamanhos dos campos de Conta */
    SQLBIGINT saldo = 0;
    SQLLEN ind = 0;
    SQLRETURN r;
    int res = -1;

    if (!st) return -1;
    if (falhou(SQLPrepare(st, (SQLCHAR *)sql, SQL_NTS), st, "preparar busca por CPF")) goto fim;
    SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, 11, 0, (SQLPOINTER)cpf11, 0, NULL);
    if (falhou(SQLExecute(st), st, "buscar por CPF")) goto fim;

    r = SQLFetch(st);
    if (r == SQL_NO_DATA) { res = 0; goto fim; }
    if (falhou(r, st, "ler cadastro")) goto fim;

    SQLGetData(st, 1, SQL_C_WCHAR, nome, sizeof nome, &ind);
    SQLGetData(st, 2, SQL_C_CHAR, nasc, sizeof nasc, &ind);
    SQLGetData(st, 3, SQL_C_CHAR, tel, sizeof tel, &ind);
    SQLGetData(st, 4, SQL_C_CHAR, numero, sizeof numero, &ind);
    SQLGetData(st, 5, SQL_C_SBIGINT, &saldo, 0, &ind);
    SQLGetData(st, 6, SQL_C_CHAR, abertura, sizeof abertura, &ind);

    memset(c, 0, sizeof *c);
    w_para_u8(nome, c->nome, sizeof c->nome);
    snprintf(c->cpf, sizeof c->cpf, "%s", cpf11);
    snprintf(c->nasc, sizeof c->nasc, "%s", nasc);
    snprintf(c->tel, sizeof c->tel, "%s", tel);
    snprintf(c->numero, sizeof c->numero, "%s", numero);
    snprintf(c->abertura, sizeof c->abertura, "%s", abertura);
    c->saldo = (centavos)saldo;
    c->persistida = 1;                         /* senha so no banco (hash): c->pin fica vazio */
    res = 1;
fim:
    libera(st);
    return res;
}

int db_proxima_sequencia(unsigned long long *seq)
{
    SQLHSTMT st = novo_stmt();
    SQLBIGINT v = 0;
    SQLLEN ind = 0;
    int ok = 0;
    if (!st) return 0;
    if (falhou(SQLExecDirect(st, (SQLCHAR *)"SELECT NEXT VALUE FOR dbo.SEQ_NUMERO_CONTA", SQL_NTS), st, "proximo numero de conta")) goto fim;
    if (falhou(SQLFetch(st), st, "ler numero de conta")) goto fim;
    SQLGetData(st, 1, SQL_C_SBIGINT, &v, 0, &ind);
    *seq = (unsigned long long)v;
    ok = 1;
fim:
    libera(st);
    return ok;
}

int db_conta_criar(const Conta *c, const char *pin4)
{
    static const char sql[] =
        "SET XACT_ABORT ON; SET NOCOUNT ON; "
        "DECLARE @salt CHAR(32) = CONVERT(CHAR(32), CRYPT_GEN_RANDOM(16), 2); "
        "BEGIN TRAN; "
        "INSERT INTO dbo.CLIENTE (nome, cpf, data_nascimento, telefone, senha_salt, senha_hash) "
        "VALUES (?, ?, ?, ?, @salt, CONVERT(CHAR(64), HASHBYTES('SHA2_256', CONCAT(@salt, ?)), 2)); "
        "INSERT INTO dbo.CONTA (id_cliente, numero, saldo, data_abertura) "
        "VALUES (SCOPE_IDENTITY(), ?, 0, ?); "
        "COMMIT;";
    SQLHSTMT st = novo_stmt();
    SQLWCHAR nome[101];
    char nasc[11], abertura[11], pin[8], numero[24];
    int ok = 0;

    if (!st) return 0;
    if (!u8_para_w(c->nome, nome, 101)) goto fim;
    data_iso(c->nasc, nasc);
    data_iso(c->abertura, abertura);
    snprintf(pin, sizeof pin, "%s", pin4);
    snprintf(numero, sizeof numero, "%s", c->numero);

    if (falhou(SQLPrepare(st, (SQLCHAR *)sql, SQL_NTS), st, "preparar cadastro")) goto fim;
    SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, 100, 0, nome, 0, NULL);
    SQLBindParameter(st, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, 11, 0, (SQLPOINTER)c->cpf, 0, NULL);
    SQLBindParameter(st, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 10, 0, nasc, 0, NULL);
    SQLBindParameter(st, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 11, 0, (SQLPOINTER)c->tel, 0, NULL);
    SQLBindParameter(st, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 4, 0, pin, 0, NULL);
    SQLBindParameter(st, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 20, 0, numero, 0, NULL);
    SQLBindParameter(st, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 10, 0, abertura, 0, NULL);
    if (falhou(SQLExecute(st), st, "gravar cadastro")) goto fim;
    ok = 1;
fim:
    memset(pin, 0, sizeof pin);
    libera(st);
    return ok;
}

int db_pin_confere(const char *cpf11, const char *pin4)
{
    static const char sql[] =
        "SELECT COUNT(*) FROM dbo.CLIENTE WHERE cpf = ? "
        "AND senha_hash = CONVERT(CHAR(64), HASHBYTES('SHA2_256', CONCAT(senha_salt, ?)), 2)";
    SQLHSTMT st = novo_stmt();
    SQLINTEGER n = 0;
    SQLLEN ind = 0;
    char pin[8];
    int res = -1;

    if (!st) return -1;
    snprintf(pin, sizeof pin, "%s", pin4);
    if (falhou(SQLPrepare(st, (SQLCHAR *)sql, SQL_NTS), st, "preparar conferencia de senha")) goto fim;
    SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, 11, 0, (SQLPOINTER)cpf11, 0, NULL);
    SQLBindParameter(st, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 4, 0, pin, 0, NULL);
    if (falhou(SQLExecute(st), st, "conferir senha")) goto fim;
    if (falhou(SQLFetch(st), st, "ler resultado da senha")) goto fim;
    SQLGetData(st, 1, SQL_C_SLONG, &n, 0, &ind);
    res = n > 0 ? 1 : 0;
fim:
    memset(pin, 0, sizeof pin);
    libera(st);
    return res;
}

==============================================================================
=== ARQUIVO: src/bot.h
==============================================================================
/* bot.h - Bia, assistente virtual do Facility Bank (maquina de estados de conversa) */
#ifndef BOT_H
#define BOT_H

#include "banco.h"

typedef enum {
    S_INICIO, S_ACESSO, S_CPF, S_PIN, S_MENU, S_MAIS,
    S_CAD_NOME, S_CAD_CPF, S_CAD_NASC, S_CAD_TEL, S_CAD_SENHA, S_CAD_SENHA2, S_CAD_CONFIRMA,
    S_EXTRATO,
    S_PIX_CHAVE, S_TED_ESCOLHE, S_TX_VALOR,
    S_BOL_ESCOLHE, S_BOL_CODIGO,
    S_TX_CONFIRMA, S_TX_GOLPE, S_TX_ACOMP, S_TX_PIN, S_TX_SMS,
    S_CARTAO, S_CARTAO_CONF, S_CARTAO_PIN,
    S_BEN_MENU, S_BEN_CHAVE, S_BEN_APELIDO, S_BEN_PIN, S_BEN_REMOVE, S_BEN_REMOVE_CONF,
    S_ACOMP_MENU, S_ACOMP_NOME, S_ACOMP_TEL, S_ACOMP_LIMITE, S_ACOMP_PIN,
    S_ALERTA_SUSP, S_ALERTA_SUSP_PIN, S_ALERTA_BOLETO, S_ALERTA_INFO,
    S_OFERTA_HUMANO, S_HUMANO
} Estado;

typedef enum { TX_PIX, TX_TED, TX_BOLETO } TxTipo;

typedef enum { PEND_NADA, PEND_BOLETO, PEND_EXTRATO } Pendencia;

/* Eventos em que o BOT fala primeiro (notificacoes e alertas) */
typedef enum { EV_SUSPEITO, EV_BOLETO_VENCE, EV_PIX_RECEBIDO, EV_FATURA, EV_TOTAL } Evento;

typedef struct {
    TxTipo   tipo;
    char     dest_nome[40];
    char     dest_chave[48];
    centavos valor;
    int      boleto_idx;
    int      dest_novo;       /* destinatario fora da lista de beneficiarios */
    int      dest_recente;    /* conta destino aberta ha pouco tempo */
    int      golpe_ok, acomp_ok, pin_ok, sms_ok;   /* verificacoes ja cumpridas */
} Tx;

typedef struct {
    Estado   estado;
    Conta   *conta;           /* NULL = nao identificado */
    Conta   *conta_pend;      /* CPF informado, aguardando senha */
    Conta   *alvo;            /* conta da notificacao (numero cadastrado) */
    int      tentativas;      /* erros seguidos no passo atual */
    int      incompreendidas; /* mensagens nao entendidas seguidas */
    Tx       tx;
    Pendencia pend;
    int      pend_idx;
    char     tmp_nome[40], tmp_chave[48], tmp_apelido[24], tmp_tel[16];
    char     cad_cpf[12], cad_nasc[12], cad_pin[5];   /* cadastro em andamento (nome e celular: tmp_nome/tmp_tel) */
    centavos tmp_valor;
    int      tmp_n;
    char     topico[32];      /* assunto atual, repassado ao atendente */
    char     motivo[96];      /* motivo do encaminhamento (nunca inclui senha/CPF) */
    char     alerta_nome[40];
    centavos alerta_valor;
} Bot;

void bot_saida_buffer(char *buf, size_t cap);      /* NULL = escreve no console */
void bot_init(Bot *b);
void bot_processar(Bot *b, const char *mensagem);  /* mensagem do cliente */
void bot_evento(Bot *b, Evento ev);                /* o bot fala primeiro */
int  bot_evento_por_nome(const char *nome);        /* -1 se invalido */
void bot_aviso_inatividade(Bot *b);
void bot_inatividade(Bot *b);                      /* encerra a sessao */

#endif

==============================================================================
=== ARQUIVO: src/bot.c
==============================================================================
/* bot.c - Bia, assistente virtual do Facility Bank.
 *
 * Regras de design (publico idoso, canal WhatsApp):
 *  - nunca ficar em silencio: mensagem nao entendida reapresenta as opcoes;
 *  - no maximo 3 botoes por pergunta de escolha (limite de "reply buttons");
 *    menus maiores sao "lista" (ate 10 itens) e o menu e dividido em 2 niveis;
 *  - VOLTAR, CANCELAR, MENU, ATENDENTE e SAIR funcionam em qualquer passo;
 *  - acoes que movimentam dinheiro: resumo -> confirmacao -> verificacoes extras;
 *  - acoes de PROTECAO (bloquear cartao, "nao fui eu") nao exigem senha;
 *    acoes de LIBERACAO exigem.
 */
#include "bot.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIMITE_OPERACAO 500000    /* R$ 5.000,00 por Pix/TED */
#define LIMITE_VERIF    100000    /* R$ 1.000,00: acima disso pede codigo SMS e, se o
                                     destinatario e novo, pergunta anti-golpe */
#define MAX_TENT        3
#define MAX_LISTA       8         /* itens de beneficiario mostrados por vez */

/* DEMO: codigos fixos para os roteiros automaticos. Em producao: gerar com CSPRNG,
 * validade curta, uso unico, e enviar pelo gateway de SMS/WhatsApp. */
#define COD_SMS   "123456"
#define COD_ACOMP "654321"

/* ------------------------------------------------------------------ saida */

/* Saida: stdout (console) ou um buffer (servidor web). */
static char  *g_buf;
static size_t g_cap, g_len;

void bot_saida_buffer(char *buf, size_t cap)
{
    g_buf = buf; g_cap = cap; g_len = 0;
    if (buf && cap) buf[0] = '\0';
}

static void emitv(const char *fmt, va_list ap)
{
    int n;
    if (!g_buf) { vprintf(fmt, ap); return; }
    if (g_len + 1 >= g_cap) return;
    n = vsnprintf(g_buf + g_len, g_cap - g_len, fmt, ap);
    if (n > 0) g_len += (size_t)n < g_cap - g_len ? (size_t)n : g_cap - g_len - 1;
}

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap);
    va_end(ap);
}
static void say(const char *fmt, ...)
{
    va_list ap;
    emit("\nBia: ");
    va_start(ap, fmt);
    emitv(fmt, ap);
    va_end(ap);
    emit("\n");
}

static void opt(int n, const char *txt) { emit("   %d) %s\n", n, txt); }

static void sim(const char *fmt, ...)    /* mensagens que, em producao, vao a OUTRO canal */
{
    va_list ap;
    emit("   [SIMULAÇÃO] ");
    va_start(ap, fmt);
    emitv(fmt, ap);
    va_end(ap);
    emit("\n");
}

/* banco de dados fora do ar: diz o que aconteceu, o que NAO aconteceu e o que fazer (C07) */
static void aviso_indisponivel(void)
{
    say("Não consegui consultar o cadastro agora, mas nada foi feito na sua conta. "
        "Tente de novo em instantes ou digite ATENDENTE para falar com uma pessoa.");
}

/* ---------------------------------------------------------------- entrada */

static char sem_acento(unsigned char b)
{
    if (b >= 0x80 && b <= 0x9F) b = (unsigned char)(b + 0x20);   /* maiuscula -> minuscula */
    if (b >= 0xA0 && b <= 0xA4) return 'a';
    if (b == 0xA7)              return 'c';
    if (b >= 0xA8 && b <= 0xAA) return 'e';
    if (b == 0xAC || b == 0xAD) return 'i';
    if (b >= 0xB2 && b <= 0xB5) return 'o';
    if (b == 0xB9 || b == 0xBA) return 'u';
    return 0;
}

/* minusculas, sem acento (UTF-8 latino), sem espacos nas pontas */
static void norm(const char *src, char *dst, size_t n)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t j = 0;
    while (*p && isspace(*p)) p++;
    for (; *p && j + 1 < n; p++) {
        if (*p == 0xC3 && p[1]) {
            char c = sem_acento(p[1]);
            p++;
            if (c) dst[j++] = c;
            continue;
        }
        dst[j++] = (char)tolower(*p);
    }
    while (j > 0 && isspace((unsigned char)dst[j - 1])) j--;
    dst[j] = '\0';
}

static void aparar(const char *src, char *dst, size_t n)   /* trim, mantendo maiusculas */
{
    size_t len;
    while (*src && isspace((unsigned char)*src)) src++;
    snprintf(dst, n, "%s", src);
    len = strlen(dst);
    while (len > 0 && isspace((unsigned char)dst[len - 1])) dst[--len] = '\0';
}

static int so_digitos(const char *src, char *dst, size_t n)
{
    size_t j = 0;
    for (; *src && j + 1 < n; src++)
        if (isdigit((unsigned char)*src)) dst[j++] = *src;
    dst[j] = '\0';
    return (int)j;
}

static int igual(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int tem(const char *a, const char *b)   { return strstr(a, b) != NULL; }

static int escolha(const char *n, int max)    /* "2" -> 2; invalido -> -1 */
{
    char *fim;
    long v;
    if (!*n) return -1;
    v = strtol(n, &fim, 10);
    if (*fim != '\0' || v < 1 || v > max) return -1;
    return (int)v;
}

static int eh_humano(const char *n)
{
    return tem(n, "atendente") || tem(n, "humano") || igual(n, "pessoa") ||
           (tem(n, "falar com") && tem(n, "pessoa"));
}

/* chave Pix digitada -> forma canonica (so digitos p/ CPF e celular; minusculas p/ resto) */
static void normaliza_chave(const char *in, char *out, size_t n)
{
    char t[64];
    const char *p;
    int so_num = 1;
    size_t i;
    aparar(in, t, sizeof t);
    for (p = t; *p; p++)
        if (!strchr("0123456789 ()-.+", *p)) { so_num = 0; break; }
    if (so_num && *t) {
        int len = so_digitos(t, out, n);
        if (len == 13 && strncmp(out, "55", 2) == 0) memmove(out, out + 2, 12);   /* +55 */
        return;
    }
    for (i = 0; t[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)t[i]);
    out[i] = '\0';
}

static void mascarar(const char *k, char *out, size_t n)
{
    size_t len = strlen(k);
    int digitos = 1;
    size_t i;
    for (i = 0; i < len; i++) if (!isdigit((unsigned char)k[i])) digitos = 0;

    if (strchr(k, '@')) {
        snprintf(out, n, "%.2s***%s", k, strchr(k, '@'));
    } else if (digitos && len == 11 && cpf_valido(k)) {
        snprintf(out, n, "***.%.3s.%.3s-**", k + 3, k + 6);
    } else if (digitos && len == 11) {
        snprintf(out, n, "(%.2s) *****-%s", k, k + 7);
    } else if (len > 8) {
        snprintf(out, n, "%.8s...", k);
    } else {
        snprintf(out, n, "%s", k);
    }
}

static const char *primeiro_nome(const Conta *c)
{
    static char buf[40];
    size_t i = 0;
    while (c->nome[i] && c->nome[i] != ' ' && i < sizeof buf - 1) { buf[i] = c->nome[i]; i++; }
    buf[i] = '\0';
    return buf;
}

/* --------------------------------------------------------- declaracoes */

static void pergunta(Bot *b);
static void ir(Bot *b, Estado e);
static void menu_principal(Bot *b, int primeira);
static void handoff(Bot *b, const char *motivo);
static void oferta_humano(Bot *b, const char *motivo);
static void proxima_verificacao(Bot *b);
static void prepara_boleto(Bot *b, int idx);

static int g_incomp_anterior;     /* contagem de "nao entendi" antes da mensagem atual */

static void reset_tx(Bot *b)
{
    TxTipo t = b->tx.tipo;
    memset(&b->tx, 0, sizeof b->tx);
    b->tx.tipo = t;
    b->tx.boleto_idx = -1;
}

static void topico(Bot *b, const char *t) { snprintf(b->topico, sizeof b->topico, "%s", t); }

static void limpa_cadastro(Bot *b)       /* descarta os dados (e a senha) de um cadastro em andamento */
{
    memset(b->cad_cpf, 0, sizeof b->cad_cpf);
    memset(b->cad_nasc, 0, sizeof b->cad_nasc);
    memset(b->cad_pin, 0, sizeof b->cad_pin);
    memset(b->tmp_nome, 0, sizeof b->tmp_nome);
    memset(b->tmp_tel, 0, sizeof b->tmp_tel);
}

static void limpa_sessao(Bot *b)
{
    limpa_cadastro(b);
    b->conta = NULL;
    b->conta_pend = NULL;
    b->pend = PEND_NADA;
    b->tentativas = 0;
    b->incompreendidas = 0;
    b->topico[0] = '\0';
    reset_tx(b);
}

/* ------------------------------------------------------------ seguranca */

/* 1 = senha certa | 0 = errada (ja avisou) | -1 = excedeu tentativas */
static int checa_pin(Bot *b, const Conta *c, const char *in)
{
    char d[16];
    int n = so_digitos(in, d, sizeof d);
    if (n == 4) {
        int ok = banco_pin_confere(c, d);
        if (ok < 0) { aviso_indisponivel(); return 0; }       /* banco fora do ar: nao conta como erro de senha */
        if (ok) { b->tentativas = 0; return 1; }
    }
    if (++b->tentativas >= MAX_TENT) return -1;
    say("Essa senha não confere. Você ainda tem %d tentativa(s). Digite os 4 números da sua senha.",
        MAX_TENT - b->tentativas);
    return 0;
}

static void falha_seguranca(Bot *b, const char *motivo)
{
    say("Você errou a senha %d vezes. Para proteger sua conta, encerrei o acesso por aqui e nada foi feito.",
        MAX_TENT);
    limpa_sessao(b);
    oferta_humano(b, motivo);
}

/* ------------------------------------------------------------ atendente */

static void handoff(Bot *b, const char *motivo)
{
    static int contador = 0;
    if (motivo != b->motivo) snprintf(b->motivo, sizeof b->motivo, "%s", motivo);
    say("Tudo bem, vou chamar uma pessoa do banco para te atender.\n"
        "Protocolo: ATD-%06d\n"
        "Já passei a ela um resumo (assunto: %s; motivo: %s), então você não precisa repetir.\n"
        "Espera estimada: cerca de 5 minutos. Pode ir escrevendo aqui mesmo.\n"
        "Para voltar ao assistente, digite VOLTAR AO BOT.",
        ++contador, b->topico[0] ? b->topico : "não informado", motivo);
    b->estado = S_HUMANO;
}

static void oferta_humano(Bot *b, const char *motivo)
{
    snprintf(b->motivo, sizeof b->motivo, "%s", motivo);
    b->estado = S_OFERTA_HUMANO;
    pergunta(b);
}

/* ---------------------------------------------------------------- menus */

static void mostra_opcoes_menu(void)
{
    opt(1, "Ver saldo");
    opt(2, "Ver extrato");
    opt(3, "Fazer um Pix");
    opt(4, "Pagar um boleto");
    opt(5, "Meu cartão");
    opt(6, "Mais serviços");
    opt(0, "Falar com uma pessoa");
}

static void menu_principal(Bot *b, int primeira)
{
    b->estado = S_MENU;
    b->tentativas = 0;
    reset_tx(b);
    b->topico[0] = '\0';
    say(primeira ? "Como posso ajudar, %s?" : "Posso ajudar em mais alguma coisa, %s?",
        primeiro_nome(b->conta));
    mostra_opcoes_menu();
}

static void ir(Bot *b, Estado e)
{
    b->estado = e;
    b->tentativas = 0;
    pergunta(b);
}

static void pos_login(Bot *b)
{
    say("Pronto, %s! Identificação confirmada.", primeiro_nome(b->conta));
    if (b->pend == PEND_EXTRATO) {
        b->pend = PEND_NADA;
        topico(b, "Extrato");
        ir(b, S_EXTRATO);
    } else if (b->pend == PEND_BOLETO) {
        b->pend = PEND_NADA;
        topico(b, "Pagar boleto");
        prepara_boleto(b, b->pend_idx);
    } else {
        menu_principal(b, 1);
    }
}

static void fim_sessao(Bot *b)
{
    say("Até logo! Por segurança, encerrei sua sessão. Quando precisar, é só mandar um OI.");
    limpa_sessao(b);
    b->estado = S_INICIO;
}

static void cancelar_op(Bot *b)
{
    say("Tudo bem, cancelei. Nada foi feito na sua conta.");
    if (b->conta) menu_principal(b, 0);
    else { limpa_sessao(b); b->estado = S_INICIO; }
}

static void nao_entendi(Bot *b)
{
    b->incompreendidas = g_incomp_anterior + 1;
    say("Desculpe, não entendi. Vamos tentar de novo:");
    pergunta(b);
    if (b->incompreendidas >= 3)
        say("Se preferir, posso chamar uma pessoa para te ajudar. É só digitar ATENDENTE.");
}

/* --------------------------------------------------------- listas / dados */

static int lista_benef(const Conta *c)
{
    int i, mostrar = c->n_benef < MAX_LISTA ? c->n_benef : MAX_LISTA;
    char m[48];
    for (i = 0; i < mostrar; i++) {
        char linha[128];
        mascarar(c->benef[i].chave, m, sizeof m);
        snprintf(linha, sizeof linha, "%s - %s (%s)", c->benef[i].apelido, c->benef[i].nome, m);
        opt(i + 1, linha);
    }
    return mostrar;
}

static int boletos_abertos(const Conta *c, int idx[])
{
    int i, n = 0;
    for (i = 0; i < c->n_boletos; i++) if (!c->boletos[i].pago) idx[n++] = i;
    return n;
}

static const char *nome_tipo(TxTipo t)
{
    return t == TX_PIX ? "Pix" : t == TX_TED ? "Transferência (TED)" : "Pagamento de boleto";
}

static Estado inicio_do_fluxo(TxTipo t)
{
    return t == TX_PIX ? S_PIX_CHAVE : t == TX_TED ? S_TED_ESCOLHE : S_BOL_ESCOLHE;
}

static Estado anterior(const Bot *b)
{
    switch (b->estado) {
    case S_CPF:            return S_ACESSO;
    case S_PIN:            return S_CPF;
    case S_CAD_NOME:       return S_ACESSO;
    case S_CAD_CPF:        return S_CAD_NOME;
    case S_CAD_NASC:       return S_CAD_CPF;
    case S_CAD_TEL:        return S_CAD_NASC;
    case S_CAD_SENHA:      return S_CAD_TEL;
    case S_CAD_SENHA2:     return S_CAD_SENHA;
    case S_CAD_CONFIRMA:   return S_CAD_SENHA;
    case S_EXTRATO: case S_MAIS: case S_PIX_CHAVE: case S_BOL_ESCOLHE:
    case S_CARTAO:         return S_MENU;
    case S_TED_ESCOLHE:    return S_MAIS;
    case S_TX_VALOR:       return b->tx.tipo == TX_PIX ? S_PIX_CHAVE : S_TED_ESCOLHE;
    case S_BOL_CODIGO:     return S_BOL_ESCOLHE;
    case S_TX_CONFIRMA:    return b->tx.tipo == TX_BOLETO ? S_BOL_ESCOLHE : S_TX_VALOR;
    case S_TX_GOLPE: case S_TX_ACOMP: case S_TX_PIN: case S_TX_SMS: return S_TX_CONFIRMA;
    case S_CARTAO_CONF: case S_CARTAO_PIN: return S_CARTAO;
    case S_BEN_MENU:       return S_MAIS;
    case S_BEN_CHAVE: case S_BEN_REMOVE: return S_BEN_MENU;
    case S_BEN_APELIDO:    return S_BEN_CHAVE;
    case S_BEN_PIN:        return S_BEN_APELIDO;
    case S_BEN_REMOVE_CONF: return S_BEN_REMOVE;
    case S_ACOMP_MENU:     return S_MAIS;
    case S_ACOMP_NOME:     return S_ACOMP_MENU;
    case S_ACOMP_TEL:      return S_ACOMP_NOME;
    case S_ACOMP_LIMITE:   return S_ACOMP_TEL;
    case S_ACOMP_PIN:      return b->tmp_n ? S_ACOMP_LIMITE : S_ACOMP_MENU;
    default:               return b->estado;
    }
}

/* ---------------------------------------------------------- perguntas */

static void resumo_tx(Bot *b)
{
    const Tx *t = &b->tx;
    char v[32], saldo[32], m[48];
    fmt_valor(t->valor, v, sizeof v);
    fmt_valor(b->conta->saldo - t->valor, saldo, sizeof saldo);
    say("Confira os dados antes de confirmar:");
    emit("   Operação:  %s\n", nome_tipo(t->tipo));
    emit("   Para:      %s\n", t->dest_nome);
    if (t->tipo == TX_BOLETO) {
        emit("   Vencimento: %s\n", b->conta->boletos[t->boleto_idx].vencimento);
    } else {
        mascarar(t->dest_chave, m, sizeof m);
        emit("   Dados:     %s\n", m);
    }
    if (t->tipo == TX_TED) emit("   Prazo:     até 1 dia útil\n");
    emit("   Valor:     %s\n", v);
    emit("   Saldo depois: %s\n", saldo);
    opt(1, "Confirmar");
    opt(2, "Corrigir");
    opt(3, "Cancelar");
}

static void pergunta(Bot *b)
{
    Conta *c = b->conta;
    char v[32], w[32];
    int i, n, idx[MAX_BOLETOS];

    switch (b->estado) {
    case S_INICIO: break;
    case S_ACESSO:
        say("Você já é cliente do Facility Bank?");
        opt(1, "Já sou cliente");
        opt(2, "Quero me cadastrar");
        break;
    case S_CPF:
        say("Digite o seu CPF, só os números.");
        break;
    case S_CAD_NOME:
        say("Qual o seu nome completo?");
        break;
    case S_CAD_CPF:
        say("Agora digite o seu CPF, só os números.");
        break;
    case S_CAD_NASC:
        say("Qual a sua data de nascimento? Por exemplo: 25/03/1950.");
        break;
    case S_CAD_TEL:
        say("Qual o seu celular, com DDD? Por exemplo: 11 98765-4321.");
        break;
    case S_CAD_SENHA:
        say("Agora escolha uma senha de 4 números. Evite datas e sequências como 1234. "
            "O banco nunca pede essa senha por telefone.");
        break;
    case S_CAD_SENHA2:
        say("Digite de novo a mesma senha de 4 números, para confirmar.");
        break;
    case S_CAD_CONFIRMA: {
        char m[48];
        mascarar(b->cad_cpf, m, sizeof m);
        say("Confira os seus dados antes de abrir a conta:");
        emit("   Nome:       %s\n", b->tmp_nome);
        emit("   CPF:        %s\n", m);
        emit("   Nascimento: %s\n", b->cad_nasc);
        emit("   Celular:    (%.2s) %.5s-%s\n", b->tmp_tel, b->tmp_tel + 2, b->tmp_tel + 7);
        opt(1, "Confirmar e abrir a conta");
        opt(2, "Corrigir");
        opt(3, "Cancelar");
        break;
    }
    case S_PIN:
        say("Agora digite a sua senha de 4 números. O banco nunca pede essa senha por telefone.");
        break;
    case S_MENU:
        say("Escolha uma das opções (digite o número):");
        mostra_opcoes_menu();
        break;
    case S_MAIS:
        say("Mais serviços:");
        opt(1, "Transferência (TED)");
        opt(2, "Meus beneficiários");
        opt(3, "Modo acompanhante");
        opt(4, "Voltar ao menu");
        break;
    case S_EXTRATO:
        say("Quantos lançamentos quer ver?");
        opt(1, "Últimos 5");
        opt(2, "Últimos 10");
        opt(3, "Voltar ao menu");
        break;
    case S_PIX_CHAVE:
        say("Para quem é o Pix? Digite a chave (CPF, celular, e-mail ou chave aleatória)%s",
            c->n_benef ? " ou escolha um beneficiário:" : ".");
        lista_benef(c);
        break;
    case S_TED_ESCOLHE:
        say("Para quem quer transferir? Escolha um beneficiário:");
        lista_benef(c);
        break;
    case S_TX_VALOR:
        say("%s para %s. Qual o valor? Digite só números, por exemplo 150 ou 150,50.",
            nome_tipo(b->tx.tipo), b->tx.dest_nome);
        break;
    case S_BOL_ESCOLHE:
        n = boletos_abertos(c, idx);
        say(n ? "Encontrei estes boletos na sua conta. Qual quer pagar?"
              : "Não encontrei boletos abertos na sua conta. Quer digitar o código?");
        for (i = 0; i < n; i++) {
            char linha[128];
            fmt_valor(c->boletos[idx[i]].valor, v, sizeof v);
            snprintf(linha, sizeof linha, "%s - %s - vence %s",
                     c->boletos[idx[i]].cedente, v, c->boletos[idx[i]].vencimento);
            opt(i + 1, linha);
        }
        opt(n + 1, "Digitar o código de barras");
        break;
    case S_BOL_CODIGO:
        say("Digite o código do boleto (os números que ficam embaixo do código de barras).");
        break;
    case S_TX_CONFIRMA:
        resumo_tx(b);
        break;
    case S_TX_GOLPE:
        say("ATENÇÃO. Antes de continuar: este pagamento é para um destinatário novo ou de conta aberta há pouco tempo.\n"
            "Alguém te pediu isso com pressa, dizendo ser do banco, parente com número novo, ou prometendo prêmio ou lucro?");
        opt(1, "Sim, alguém me pediu");
        opt(2, "Não, a decisão foi minha");
        opt(3, "Cancelar");
        break;
    case S_TX_ACOMP:
        say("Você ativou o modo acompanhante, e este valor precisa da aprovação de %s.\n"
            "Enviei um código para o celular dessa pessoa. Peça o código a ela e digite aqui (6 números).",
            c->acomp_nome);
        sim("Mensagem para o acompanhante: \"%s quer fazer um pagamento. Código de aprovação: %s\"",
            c->nome, COD_ACOMP);
        break;
    case S_TX_PIN:
        say("Para concluir, digite a sua senha de 4 números.");
        break;
    case S_TX_SMS:
        say("Por segurança, enviei um código de 6 números por SMS para o seu celular cadastrado. Digite o código aqui.");
        sim("SMS para o celular cadastrado: \"Facility Bank: seu código é %s. Não compartilhe com ninguém.\"", COD_SMS);
        break;
    case S_CARTAO:
        say("Cartão final %s (%s).", c->cartao_final, c->cartao_bloqueado ? "BLOQUEADO" : "ativo");
        opt(1, "Ver fatura e limite");
        opt(2, c->cartao_bloqueado ? "Desbloquear cartão" : "Bloquear cartão");
        opt(3, "Voltar ao menu");
        break;
    case S_CARTAO_CONF:
        say("Bloquear o cartão final %s? Compras serão recusadas até você desbloquear.", c->cartao_final);
        opt(1, "Sim, bloquear");
        opt(2, "Não, manter ativo");
        break;
    case S_CARTAO_PIN:
        say("Para desbloquear o cartão, digite a sua senha de 4 números.");
        break;
    case S_BEN_MENU:
        say("Seus beneficiários (%d de %d).", c->n_benef, MAX_BENEF);
        opt(1, "Ver a lista");
        opt(2, "Adicionar um novo");
        opt(3, "Remover um");
        opt(4, "Voltar");
        break;
    case S_BEN_CHAVE:
        say("Digite a chave Pix da pessoa que quer adicionar (CPF, celular, e-mail ou chave aleatória).");
        break;
    case S_BEN_APELIDO:
        say("Encontrei: %s. Que apelido quer dar? Por exemplo: Filha Ana.", b->tmp_nome);
        break;
    case S_BEN_PIN:
        say("Para salvar %s na sua lista, digite a sua senha de 4 números.", b->tmp_apelido);
        break;
    case S_BEN_REMOVE:
        say("Qual beneficiário quer remover?");
        lista_benef(c);
        break;
    case S_BEN_REMOVE_CONF:
        say("Remover %s da sua lista?", c->benef[b->tmp_n].apelido);
        opt(1, "Sim, remover");
        opt(2, "Não, manter");
        break;
    case S_ACOMP_MENU:
        if (c->acomp_ativo) {
            fmt_valor(c->acomp_limite, v, sizeof v);
            say("Modo acompanhante: LIGADO. %s precisa aprovar pagamentos a partir de %s.", c->acomp_nome, v);
            opt(1, "Desligar");
        } else {
            say("Modo acompanhante: desligado. Uma pessoa de confiança aprova, com um código, os pagamentos acima do valor que você escolher.");
            opt(1, "Ligar");
        }
        opt(2, "Voltar");
        break;
    case S_ACOMP_NOME:
        say("Qual o nome da pessoa de confiança?");
        break;
    case S_ACOMP_TEL:
        say("Qual o celular dela, com DDD? Por exemplo: 11 98765-4321.");
        break;
    case S_ACOMP_LIMITE:
        say("A partir de que valor ela precisa aprovar? Por exemplo: 300.");
        break;
    case S_ACOMP_PIN:
        say(b->tmp_n ? "Para ligar o modo acompanhante, digite a sua senha de 4 números."
                     : "Para desligar o modo acompanhante, digite a sua senha de 4 números.");
        break;
    case S_ALERTA_SUSP:
        fmt_valor(b->alerta_valor, v, sizeof v);
        say("Aviso de segurança do Facility Bank.\n"
            "Seguramos por precaução um Pix de %s para \"%s\", pedido às 02:14 pelo aplicativo.\n"
            "Foi você?", v, b->alerta_nome);
        opt(1, "Fui eu, pode liberar");
        opt(2, "Não fui eu");
        opt(3, "Falar com uma pessoa");
        say("Se eu não tiver resposta, o Pix continua bloqueado por segurança.");
        break;
    case S_ALERTA_SUSP_PIN:
        say("Para liberar, digite a sua senha de 4 números.");
        break;
    case S_ALERTA_BOLETO:
        opt(1, "Pagar agora");
        opt(2, "Me lembre amanhã");
        opt(3, "Agora não");
        break;
    case S_ALERTA_INFO:
        opt(1, "Ver meu extrato");
        opt(2, "Está tudo certo, obrigado");
        break;
    case S_OFERTA_HUMANO:
        say("Quer falar com uma pessoa do banco agora?");
        opt(1, "Sim, quero");
        opt(2, "Não, obrigado");
        break;
    case S_HUMANO:
        break;
    }
    (void)w;
}

/* ------------------------------------------------- transacoes (pipeline) */

static int precisa_golpe(const Bot *b)
{
    const Tx *t = &b->tx;
    return t->tipo != TX_BOLETO &&
           (t->dest_recente || (t->dest_novo && t->valor >= LIMITE_VERIF));
}

static int precisa_acomp(const Bot *b)
{
    return b->conta->acomp_ativo && b->tx.valor >= b->conta->acomp_limite;
}

static int precisa_sms(const Bot *b)
{
    return b->tx.valor >= LIMITE_VERIF || (b->tx.tipo == TX_PIX && b->tx.dest_novo);
}

static void executar(Bot *b)
{
    Conta *c = b->conta;
    Tx *t = &b->tx;
    char desc[64], v[32], s[32];

    if (t->valor > c->saldo) {
        say("O seu saldo mudou e agora não é suficiente. Nada foi enviado.");
        menu_principal(b, 0);
        return;
    }
    switch (t->tipo) {
    case TX_PIX:    snprintf(desc, sizeof desc, "Pix enviado - %s", t->dest_nome); break;
    case TX_TED:    snprintf(desc, sizeof desc, "Transferencia - %s", t->dest_nome); break;
    default:        snprintf(desc, sizeof desc, "Boleto - %s", t->dest_nome); break;
    }
    banco_lancar(c, desc, -t->valor);
    if (t->tipo == TX_BOLETO) c->boletos[t->boleto_idx].pago = 1;

    fmt_valor(t->valor, v, sizeof v);
    fmt_valor(c->saldo, s, sizeof s);
    say("Pronto! %s de %s para %s foi concluído.\n"
        "Comprovante nº %08lX\n"
        "Saldo atual: %s",
        nome_tipo(t->tipo), v, t->dest_nome, (unsigned long)time(NULL), s);
    menu_principal(b, 0);
}

static void proxima_verificacao(Bot *b)
{
    Tx *t = &b->tx;
    if (precisa_golpe(b) && !t->golpe_ok)       ir(b, S_TX_GOLPE);
    else if (precisa_acomp(b) && !t->acomp_ok)  ir(b, S_TX_ACOMP);
    else if (!t->pin_ok)                        ir(b, S_TX_PIN);
    else if (precisa_sms(b) && !t->sms_ok)      ir(b, S_TX_SMS);
    else                                        executar(b);
}

static void prepara_boleto(Bot *b, int idx)
{
    const Boleto *bo = &b->conta->boletos[idx];
    char v[32], s[32];
    if (bo->pago) {
        say("Esse boleto já foi pago. Nada foi cobrado.");
        reset_tx(b);
        b->tx.tipo = TX_BOLETO;
        ir(b, S_BOL_ESCOLHE);
        return;
    }
    reset_tx(b);
    b->tx.tipo = TX_BOLETO;
    b->tx.boleto_idx = idx;
    b->tx.valor = bo->valor;
    snprintf(b->tx.dest_nome, sizeof b->tx.dest_nome, "%s", bo->cedente);
    if (bo->valor > b->conta->saldo) {
        fmt_valor(bo->valor, v, sizeof v);
        fmt_valor(b->conta->saldo, s, sizeof s);
        say("O saldo (%s) não cobre esse boleto de %s. Nada foi pago. "
            "Você pode escolher outro boleto ou voltar ao menu.", s, v);
        ir(b, S_BOL_ESCOLHE);
        return;
    }
    ir(b, S_TX_CONFIRMA);
}

/* ------------------------------------------------------------ handlers */

static void h_cpf(Bot *b, const char *in)
{
    char d[32];
    int n = so_digitos(in, d, sizeof d);
    Conta *c;
    if (n != 11) {
        say("O CPF tem 11 números. Pode digitar de novo?");
        if (++b->tentativas >= MAX_TENT) oferta_humano(b, "dificuldade para informar o CPF");
        return;
    }
    if (!cpf_valido(d) || !(c = banco_conta_por_cpf(d))) {
        if (cpf_valido(d) && banco_indisponivel()) { aviso_indisponivel(); return; }
        say("Não encontrei um cadastro com esse CPF. Confira os números e tente de novo.");
        if (++b->tentativas >= MAX_TENT) oferta_humano(b, "CPF não encontrado 3 vezes");
        return;
    }
    b->conta_pend = c;
    ir(b, S_PIN);
}

static void h_pin_login(Bot *b, const char *in)
{
    int r = checa_pin(b, b->conta_pend, in);
    if (r == 1) { b->conta = b->conta_pend; b->conta_pend = NULL; pos_login(b); }
    else if (r < 0) falha_seguranca(b, "senha errada 3 vezes na identificação");
}

static void h_menu(Bot *b, const char *n)
{
    int o = escolha(n, 6);
    if (o < 0) {
        if (tem(n, "saldo")) o = 1;
        else if (tem(n, "extrato")) o = 2;
        else if (tem(n, "pix")) o = 3;
        else if (tem(n, "boleto") || tem(n, "pagar")) o = 4;
        else if (tem(n, "cartao")) o = 5;
        else if (tem(n, "mais") || tem(n, "servico")) o = 6;
    }
    if (igual(n, "0")) { handoff(b, "pedido do cliente"); return; }
    switch (o) {
    case 1: {
        char s[32];
        topico(b, "Saldo");
        fmt_valor(b->conta->saldo, s, sizeof s);
        say("O seu saldo agora é %s.", s);
        menu_principal(b, 0);
        break;
    }
    case 2: topico(b, "Extrato"); ir(b, S_EXTRATO); break;
    case 3:
        topico(b, "Pix");
        if (b->conta->pix_bloqueado) {
            say("O seu Pix está bloqueado por segurança. Para liberar, preciso te passar para uma pessoa do banco.");
            oferta_humano(b, "Pix bloqueado por segurança");
            break;
        }
        reset_tx(b);
        b->tx.tipo = TX_PIX;
        ir(b, S_PIX_CHAVE);
        break;
    case 4:
        topico(b, "Pagar boleto");
        reset_tx(b);
        b->tx.tipo = TX_BOLETO;
        ir(b, S_BOL_ESCOLHE);
        break;
    case 5:
        topico(b, "Cartão");
        if (!*b->conta->cartao_final) {            /* conta nova: ainda sem cartão */
            say("Você ainda não tem um cartão do Facility Bank. Uma pessoa do banco pode te ajudar a pedir o seu.");
            oferta_humano(b, "cliente novo quer pedir cartão");
            break;
        }
        ir(b, S_CARTAO);
        break;
    case 6: ir(b, S_MAIS); break;
    default: nao_entendi(b); break;
    }
}

static void h_mais(Bot *b, const char *n)
{
    switch (escolha(n, 4)) {
    case 1:
        topico(b, "Transferência");
        if (b->conta->n_benef == 0) {
            say("Para transferir, você precisa ter um beneficiário salvo. Vou te levar para cadastrar um.");
            ir(b, S_BEN_MENU);
            return;
        }
        reset_tx(b);
        b->tx.tipo = TX_TED;
        ir(b, S_TED_ESCOLHE);
        break;
    case 2: topico(b, "Beneficiários"); ir(b, S_BEN_MENU); break;
    case 3: topico(b, "Modo acompanhante"); ir(b, S_ACOMP_MENU); break;
    case 4: menu_principal(b, 0); break;
    default: nao_entendi(b); break;
    }
}

static void h_extrato(Bot *b, const char *n)
{
    int o = escolha(n, 3), i, qtd;
    if (o < 0) { nao_entendi(b); return; }
    if (o == 3) { menu_principal(b, 0); return; }
    qtd = o == 1 ? 5 : 10;
    if (qtd > b->conta->n_extrato) qtd = b->conta->n_extrato;
    if (qtd == 0) {
        say("Você ainda não tem lançamentos na conta.");
        menu_principal(b, 0);
        return;
    }
    say("Seus últimos %d lançamentos:", qtd);
    for (i = b->conta->n_extrato - 1; i >= b->conta->n_extrato - qtd; i--) {
        const Lancamento *l = &b->conta->extrato[i];
        char v[32];
        fmt_valor(l->valor, v, sizeof v);
        emit("   %s  %-30s %14s\n", l->quando, l->descricao, v);
    }
    menu_principal(b, 0);
}

/* destinatario escolhido (de beneficiario ou chave digitada) -> pede o valor */
static void define_destino(Bot *b, const char *nome, const char *chave, int novo, int recente)
{
    snprintf(b->tx.dest_nome, sizeof b->tx.dest_nome, "%s", nome);
    snprintf(b->tx.dest_chave, sizeof b->tx.dest_chave, "%s", chave);
    b->tx.dest_novo = novo;
    b->tx.dest_recente = recente;
    ir(b, S_TX_VALOR);
}

static void h_pix_chave(Bot *b, const char *in, const char *n)
{
    char chave[64];
    ContaPix p;
    int o = escolha(n, b->conta->n_benef < MAX_LISTA ? b->conta->n_benef : MAX_LISTA);
    if (o > 0 && strlen(n) <= 2) {
        const Beneficiario *bn = &b->conta->benef[o - 1];
        int recente = 0;
        if (banco_pix_lookup(bn->chave, &p)) recente = p.conta_recente;
        define_destino(b, bn->nome, bn->chave, 0, recente);
        return;
    }
    normaliza_chave(in, chave, sizeof chave);
    if (!*chave || !banco_pix_lookup(chave, &p)) {
        say("Não encontrei essa chave. Confira se digitou certinho, sem espaços, e tente de novo.");
        if (++b->tentativas >= MAX_TENT) {
            say("Vamos voltar ao menu para você conferir a chave com calma.");
            menu_principal(b, 0);
        }
        return;
    }
    define_destino(b, p.nome, p.chave, banco_benef_por_chave(b->conta, p.chave) < 0, p.conta_recente);
}

static void h_ted_escolhe(Bot *b, const char *n)
{
    int max = b->conta->n_benef < MAX_LISTA ? b->conta->n_benef : MAX_LISTA;
    int o = escolha(n, max);
    ContaPix p;
    if (o < 0) { nao_entendi(b); return; }
    {
        const Beneficiario *bn = &b->conta->benef[o - 1];
        int recente = banco_pix_lookup(bn->chave, &p) ? p.conta_recente : 0;
        define_destino(b, bn->nome, bn->chave, 0, recente);
    }
}

static void h_valor(Bot *b, const char *in)
{
    centavos v;
    char s[32], l[32];
    if (!parse_valor(in, &v)) {
        say("Não entendi o valor. Digite só números, por exemplo 150 ou 150,50.");
        return;
    }
    if (v > LIMITE_OPERACAO) {
        fmt_valor(LIMITE_OPERACAO, l, sizeof l);
        say("O limite por operação é %s. Digite um valor menor ou digite CANCELAR.", l);
        return;
    }
    if (v > b->conta->saldo) {
        fmt_valor(b->conta->saldo, s, sizeof s);
        say("O saldo não é suficiente. Você tem %s. Digite um valor menor ou digite CANCELAR.", s);
        return;
    }
    b->tx.valor = v;
    ir(b, S_TX_CONFIRMA);
}

static void h_bol_escolhe(Bot *b, const char *n)
{
    int idx[MAX_BOLETOS], k = boletos_abertos(b->conta, idx);
    int o = escolha(n, k + 1);
    if (o < 0) { nao_entendi(b); return; }
    if (o == k + 1) { ir(b, S_BOL_CODIGO); return; }
    prepara_boleto(b, idx[o - 1]);
}

static void h_bol_codigo(Bot *b, const char *in)
{
    char d[64];
    int n = so_digitos(in, d, sizeof d), i;
    if (n < 44 || n > 48) {
        say("O código do boleto tem de 44 a 48 números. Confira e digite de novo.");
        return;
    }
    i = banco_boleto_por_codigo(b->conta, d);
    if (i < 0) {
        say("Não encontrei esse boleto. Confira os números ou escolha um da lista.");
        if (++b->tentativas >= MAX_TENT) ir(b, S_BOL_ESCOLHE);
        return;
    }
    if (b->conta->boletos[i].pago) {
        say("Esse boleto já foi pago. Nada foi cobrado.");
        ir(b, S_BOL_ESCOLHE);
        return;
    }
    prepara_boleto(b, i);
}

static void h_confirma(Bot *b, const char *n)
{
    switch (escolha(n, 3)) {
    case 1: proxima_verificacao(b); break;
    case 2: {
        TxTipo t = b->tx.tipo;
        reset_tx(b);
        b->tx.tipo = t;
        ir(b, inicio_do_fluxo(t));
        break;
    }
    case 3: cancelar_op(b); break;
    default:
        if (igual(n, "confirmar") || igual(n, "sim")) proxima_verificacao(b);
        else nao_entendi(b);
    }
}

static void h_golpe(Bot *b, const char *n)
{
    switch (escolha(n, 3)) {
    case 1:
        say("Fez bem em me contar. Esse tipo de pedido costuma ser golpe, então não vou fazer esse pagamento. "
            "Nenhum valor saiu da sua conta.");
        reset_tx(b);
        oferta_humano(b, "cliente relatou pedido de pagamento com pressa (possível golpe)");
        break;
    case 2: b->tx.golpe_ok = 1; proxima_verificacao(b); break;
    case 3: cancelar_op(b); break;
    default: nao_entendi(b);
    }
}

static void h_codigo(Bot *b, const char *in, const char *esperado, int *flag, const char *falha)
{
    char d[16];
    int n = so_digitos(in, d, sizeof d);
    if (n == 6 && strcmp(d, esperado) == 0) {
        *flag = 1;
        b->tentativas = 0;
        proxima_verificacao(b);
        return;
    }
    if (++b->tentativas >= MAX_TENT) {
        say("O código não confere e as tentativas acabaram. Cancelei a operação, nada foi feito.");
        reset_tx(b);
        oferta_humano(b, falha);
        return;
    }
    say("Esse código não confere. Você ainda tem %d tentativa(s).", MAX_TENT - b->tentativas);
}

static void h_cartao(Bot *b, const char *n)
{
    Conta *c = b->conta;
    switch (escolha(n, 3)) {
    case 1: {
        char f[32], l[32];
        fmt_valor(c->cartao_fatura, f, sizeof f);
        fmt_valor(c->cartao_limite - c->cartao_fatura, l, sizeof l);
        say("Fatura atual: %s, vence em %s. Limite disponível: %s.", f, c->cartao_venc, l);
        pergunta(b);
        break;
    }
    case 2: ir(b, c->cartao_bloqueado ? S_CARTAO_PIN : S_CARTAO_CONF); break;
    case 3: menu_principal(b, 0); break;
    default: nao_entendi(b);
    }
}

static void h_cartao_conf(Bot *b, const char *n)
{
    switch (escolha(n, 2)) {
    case 1:
        b->conta->cartao_bloqueado = 1;
        say("Pronto, cartão bloqueado. Nenhuma compra será aprovada até você desbloquear.");
        ir(b, S_CARTAO);
        break;
    case 2: ir(b, S_CARTAO); break;
    default: nao_entendi(b);
    }
}

static void h_ben_menu(Bot *b, const char *n)
{
    switch (escolha(n, 4)) {
    case 1:
        if (b->conta->n_benef == 0) say("Você ainda não tem beneficiários salvos.");
        else { say("Seus beneficiários:"); lista_benef(b->conta); }
        pergunta(b);
        break;
    case 2:
        if (b->conta->n_benef >= MAX_BENEF) {
            say("A lista está cheia (%d). Remova alguém para adicionar outra pessoa.", MAX_BENEF);
            pergunta(b);
        } else ir(b, S_BEN_CHAVE);
        break;
    case 3:
        if (b->conta->n_benef == 0) { say("Não há ninguém para remover."); pergunta(b); }
        else ir(b, S_BEN_REMOVE);
        break;
    case 4: ir(b, S_MAIS); break;
    default: nao_entendi(b);
    }
}

static void h_ben_chave(Bot *b, const char *in)
{
    char chave[64];
    ContaPix p;
    normaliza_chave(in, chave, sizeof chave);
    if (!*chave || !banco_pix_lookup(chave, &p)) {
        say("Não encontrei essa chave. Confira se digitou certinho e tente de novo.");
        if (++b->tentativas >= MAX_TENT) ir(b, S_BEN_MENU);
        return;
    }
    if (banco_benef_por_chave(b->conta, p.chave) >= 0) {
        say("Essa pessoa já está na sua lista.");
        ir(b, S_BEN_MENU);
        return;
    }
    snprintf(b->tmp_nome, sizeof b->tmp_nome, "%s", p.nome);
    snprintf(b->tmp_chave, sizeof b->tmp_chave, "%s", p.chave);
    ir(b, S_BEN_APELIDO);
}

static void h_ben_apelido(Bot *b, const char *in)
{
    char a[64];
    size_t len;
    aparar(in, a, sizeof a);
    len = strlen(a);
    if (len < 2 || len >= sizeof b->tmp_apelido) {
        say("O apelido precisa ter de 2 a %d letras. Tente outro.", (int)sizeof b->tmp_apelido - 1);
        return;
    }
    snprintf(b->tmp_apelido, sizeof b->tmp_apelido, "%s", a);
    ir(b, S_BEN_PIN);
}

static void h_ben_remove(Bot *b, const char *n)
{
    int max = b->conta->n_benef < MAX_LISTA ? b->conta->n_benef : MAX_LISTA;
    int o = escolha(n, max);
    if (o < 0) { nao_entendi(b); return; }
    b->tmp_n = o - 1;
    ir(b, S_BEN_REMOVE_CONF);
}

static void h_ben_remove_conf(Bot *b, const char *n)
{
    switch (escolha(n, 2)) {
    case 1:
        banco_benef_remove(b->conta, b->tmp_n);
        say("Pronto, removido da sua lista.");
        ir(b, S_BEN_MENU);
        break;
    case 2: ir(b, S_BEN_MENU); break;
    default: nao_entendi(b);
    }
}

static void h_acomp_menu(Bot *b, const char *n)
{
    switch (escolha(n, 2)) {
    case 1:
        b->tmp_n = !b->conta->acomp_ativo;       /* 1 = ligar, 0 = desligar */
        ir(b, b->tmp_n ? S_ACOMP_NOME : S_ACOMP_PIN);
        break;
    case 2: ir(b, S_MAIS); break;
    default: nao_entendi(b);
    }
}

static void h_acomp_nome(Bot *b, const char *in)
{
    char a[64];
    size_t len;
    aparar(in, a, sizeof a);
    len = strlen(a);
    if (len < 2 || len >= sizeof b->tmp_nome) {
        say("O nome precisa ter de 2 a %d letras. Tente de novo.", (int)sizeof b->tmp_nome - 1);
        return;
    }
    snprintf(b->tmp_nome, sizeof b->tmp_nome, "%s", a);
    ir(b, S_ACOMP_TEL);
}

static void h_acomp_tel(Bot *b, const char *in)
{
    char d[32];
    int n = so_digitos(in, d, sizeof d);
    if (n != 10 && n != 11) {
        say("O celular precisa ter DDD e número (10 ou 11 dígitos). Confira e digite de novo.");
        return;
    }
    snprintf(b->tmp_tel, sizeof b->tmp_tel, "%s", d);
    ir(b, S_ACOMP_LIMITE);
}

static void h_acomp_limite(Bot *b, const char *in)
{
    centavos v;
    if (!parse_valor(in, &v)) {
        say("Não entendi o valor. Digite só números, por exemplo 300.");
        return;
    }
    b->tmp_valor = v;
    ir(b, S_ACOMP_PIN);
}

static void h_alerta_susp(Bot *b, const char *n)
{
    Conta *c = b->alvo;
    switch (escolha(n, 3)) {
    case 1: ir(b, S_ALERTA_SUSP_PIN); break;
    case 2:
        c->pix_bloqueado = 1;
        c->cartao_bloqueado = 1;
        say("Obrigada por avisar. Já bloqueei o seu Pix e o seu cartão para proteger o seu dinheiro. "
            "O Pix de que falamos NÃO será feito.\n"
            "Abri uma contestação, protocolo CTS-%06d. Um especialista vai analisar com prioridade.",
            (int)(time(NULL) % 1000000));
        oferta_humano(b, "cliente não reconhece Pix suspeito; Pix e cartão bloqueados");
        break;
    case 3: handoff(b, "cliente quer confirmar alerta de movimentação suspeita"); break;
    default: nao_entendi(b);
    }
}

static void h_alerta_susp_pin(Bot *b, const char *in)
{
    int r = checa_pin(b, b->alvo, in);
    if (r == 1) {
        char v[32], desc[64];
        Conta *c = b->alvo;
        fmt_valor(b->alerta_valor, v, sizeof v);
        if (b->alerta_valor <= c->saldo) {
            snprintf(desc, sizeof desc, "Pix enviado - %s", b->alerta_nome);
            banco_lancar(c, desc, -b->alerta_valor);
            say("Liberado. O Pix de %s para %s foi concluído. Se algo parecer estranho depois, é só me avisar.",
                v, b->alerta_nome);
        } else {
            say("O saldo não é mais suficiente para esse Pix, então ele não foi feito.");
        }
        b->estado = S_INICIO;
    } else if (r < 0) {
        say("A senha não confere e as tentativas acabaram. Mantive o Pix bloqueado por segurança.");
        oferta_humano(b, "senha errada ao liberar Pix suspeito");
    }
}

static void h_alerta_boleto(Bot *b, const char *n)
{
    switch (escolha(n, 3)) {
    case 1:
        if (b->conta) {
            topico(b, "Pagar boleto");
            prepara_boleto(b, b->pend_idx);
        } else {
            b->pend = PEND_BOLETO;
            say("Certo! Primeiro preciso confirmar quem é você.");
            ir(b, S_CPF);
        }
        break;
    case 2:
    case 3:
        say(escolha(n, 3) == 2 ? "Combinado, eu te lembro amanhã." : "Tudo bem. Quando quiser pagar, é só me chamar.");
        if (b->conta) menu_principal(b, 0); else b->estado = S_INICIO;
        break;
    default: nao_entendi(b);
    }
}

static void h_alerta_info(Bot *b, const char *n)
{
    switch (escolha(n, 2)) {
    case 1:
        if (b->conta) { topico(b, "Extrato"); ir(b, S_EXTRATO); }
        else { b->pend = PEND_EXTRATO; say("Primeiro preciso confirmar quem é você."); ir(b, S_CPF); }
        break;
    case 2:
        say("Por nada! Qualquer coisa, é só mandar um OI.");
        if (b->conta) menu_principal(b, 0); else b->estado = S_INICIO;
        break;
    default: nao_entendi(b);
    }
}

static void h_oferta_humano(Bot *b, const char *n)
{
    switch (escolha(n, 2)) {
    case 1: handoff(b, b->motivo); break;
    case 2:
        say("Tudo bem. Se precisar, é só chamar.");
        if (b->conta) menu_principal(b, 0); else { limpa_sessao(b); b->estado = S_INICIO; }
        break;
    default: nao_entendi(b);
    }
}

static void h_humano(Bot *b, const char *n)
{
    if (tem(n, "voltar ao bot") || tem(n, "voltar para o bot") || igual(n, "encerrar")) {
        say("Certo, voltei a ajudar por aqui.");
        if (b->conta) menu_principal(b, 0);
        else { say("Digite o seu CPF, só os números, para eu confirmar quem é você."); b->estado = S_CPF; }
    } else {
        say("(Sua mensagem foi enviada ao atendente. Aguarde um pouquinho, por favor.)");
    }
}

/* ------------------------------------------------ acesso e cadastro */

static int ja_cliente(const char *n)
{
    return tem(n, "cliente") || tem(n, "entrar") || tem(n, "acessar") ||
           tem(n, "ja tenho") || tem(n, "ja sou");
}

static int quer_cadastro(const char *n)
{
    return tem(n, "cadastr") || tem(n, "abrir") || tem(n, "novo") ||
           tem(n, "nao sou") || tem(n, "nao tenho");
}

static int em_cadastro(Estado e)
{
    return e == S_CAD_NOME || e == S_CAD_CPF || e == S_CAD_NASC || e == S_CAD_TEL ||
           e == S_CAD_SENHA || e == S_CAD_SENHA2 || e == S_CAD_CONFIRMA;
}

static void inicia_login(Bot *b)
{
    say("Por segurança, primeiro vou confirmar quem é você.\n"
        "Digite o seu CPF, só os números.");
    b->estado = S_CPF;
    b->tentativas = 0;
}

static void inicia_cadastro(Bot *b)
{
    limpa_cadastro(b);
    topico(b, "Cadastro");
    say("Que bom ter você por aqui! Vou abrir a sua conta em poucos passos. "
        "Se quiser desistir, digite CANCELAR.");
    ir(b, S_CAD_NOME);
}

static void cancela_cadastro(Bot *b)
{
    say("Tudo bem, cancelei o cadastro. Nenhum dado foi guardado. "
        "Quando quiser abrir a sua conta, é só mandar um OI.");
    limpa_sessao(b);
    b->estado = S_INICIO;
}

/* erro de digitação em um passo do cadastro: no 3º seguido, oferece uma pessoa */
static void falha_cadastro(Bot *b, const char *motivo)
{
    if (++b->tentativas >= MAX_TENT) {
        limpa_cadastro(b);
        oferta_humano(b, motivo);
    }
}

static int nome_valido(const char *s, size_t cap)
{
    size_t i, len = strlen(s);
    int palavras = 0, letras = 0, em_palavra = 0;
    if (len < 5 || len >= cap) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') { em_palavra = 0; continue; }
        if (isalpha(c) || c >= 0x80) letras++;                 /* >= 0x80: letras acentuadas (UTF-8) */
        else if (c != '\'' && c != '-') return 0;
        if (!em_palavra) { palavras++; em_palavra = 1; }
    }
    return palavras >= 2 && letras >= 4;
}

static int so_numeros(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* senha de 4 digitos: recusa repetida (1111), em sequencia (1234, 4321) e ligada ao nascimento */
static int senha_fraca(const Bot *b, const char *p)
{
    int i, repete = 1, sobe = 1, desce = 1;
    for (i = 1; i < 4; i++) {
        if (p[i] != p[0]) repete = 0;
        if (p[i] - p[i - 1] != 1) sobe = 0;
        if (p[i - 1] - p[i] != 1) desce = 0;
    }
    if (repete || sobe || desce) return 1;
    if (strcmp(p, b->cad_nasc + 6) == 0) return 1;             /* ano: dd/mm/AAAA */
    return p[0] == b->cad_nasc[0] && p[1] == b->cad_nasc[1] &&
           p[2] == b->cad_nasc[3] && p[3] == b->cad_nasc[4];   /* dia e mes */
}

static void h_acesso(Bot *b, const char *n)
{
    int o = escolha(n, 2);
    if (o < 0) o = quer_cadastro(n) ? 2 : ja_cliente(n) ? 1 : -1;
    switch (o) {
    case 1: inicia_login(b); break;
    case 2: inicia_cadastro(b); break;
    default: nao_entendi(b);
    }
}

static void h_cad_nome(Bot *b, const char *in)
{
    char a[64];
    aparar(in, a, sizeof a);
    if (!nome_valido(a, sizeof b->tmp_nome)) {
        say("Preciso do nome completo, com nome e sobrenome, só com letras. Pode digitar de novo?");
        falha_cadastro(b, "dificuldade para informar o nome no cadastro");
        return;
    }
    snprintf(b->tmp_nome, sizeof b->tmp_nome, "%s", a);
    ir(b, S_CAD_CPF);
}

static void h_cad_cpf(Bot *b, const char *in)
{
    char d[32];
    int n = so_digitos(in, d, sizeof d);
    Conta *existente;
    if (n != 11 || !cpf_valido(d)) {
        say("O CPF tem 11 números e precisa ser válido. Confira e digite de novo.");
        falha_cadastro(b, "dificuldade para informar o CPF no cadastro");
        return;
    }
    existente = banco_conta_por_cpf(d);
    if (!existente && banco_indisponivel()) {      /* sem banco nao dá para saber se o CPF já tem conta */
        aviso_indisponivel();
        return;
    }
    if (existente) {
        say("Esse CPF já tem cadastro no Facility Bank, então não preciso abrir outra conta. "
            "Vamos entrar na sua.");
        limpa_cadastro(b);
        b->conta_pend = existente;
        ir(b, S_PIN);
        return;
    }
    snprintf(b->cad_cpf, sizeof b->cad_cpf, "%s", d);
    ir(b, S_CAD_NASC);
}

static void h_cad_nasc(Bot *b, const char *in)
{
    char f[12];
    int r = parse_data_nasc(in, f, sizeof f);
    if (r < 0) {
        say("Para abrir uma conta é preciso ter 18 anos ou mais. Nenhum dado foi guardado.");
        limpa_cadastro(b);
        oferta_humano(b, "cadastro de menor de 18 anos");
        return;
    }
    if (r == 0) {
        say("Não entendi a data. Digite dia, mês e ano, por exemplo 25/03/1950.");
        falha_cadastro(b, "dificuldade para informar a data de nascimento no cadastro");
        return;
    }
    snprintf(b->cad_nasc, sizeof b->cad_nasc, "%s", f);
    ir(b, S_CAD_TEL);
}

static void h_cad_tel(Bot *b, const char *in)
{
    char d[32];
    int n = so_digitos(in, d, sizeof d);
    if (n != 11 || d[0] == '0' || d[1] == '0' || d[2] != '9') {
        say("O celular precisa ter DDD e 9 números, começando por 9. Por exemplo: 11 98765-4321.");
        falha_cadastro(b, "dificuldade para informar o celular no cadastro");
        return;
    }
    snprintf(b->tmp_tel, sizeof b->tmp_tel, "%s", d);
    ir(b, S_CAD_SENHA);
}

static void h_cad_senha(Bot *b, const char *in)
{
    char p[16];
    aparar(in, p, sizeof p);
    if (strlen(p) != 4 || !so_numeros(p)) {
        say("A senha precisa ter exatamente 4 números, sem letras. Tente de novo.");
        return;
    }
    if (senha_fraca(b, p)) {
        say("Essa senha é fácil de adivinhar (números repetidos, em sequência ou ligados à sua data de nascimento). "
            "Escolha outra, por favor.");
        return;
    }
    snprintf(b->cad_pin, sizeof b->cad_pin, "%s", p);
    ir(b, S_CAD_SENHA2);
}

static void h_cad_senha2(Bot *b, const char *in)
{
    char p[16];
    aparar(in, p, sizeof p);
    if (strcmp(p, b->cad_pin) != 0) {
        memset(b->cad_pin, 0, sizeof b->cad_pin);
        say("As senhas não são iguais. Vamos escolher de novo, com calma.");
        ir(b, S_CAD_SENHA);
        return;
    }
    ir(b, S_CAD_CONFIRMA);
}

static void concluir_cadastro(Bot *b)
{
    Conta *c = banco_conta_criar(b->tmp_nome, b->cad_cpf, b->cad_nasc, b->tmp_tel, b->cad_pin);
    char s[32];
    limpa_cadastro(b);
    if (!c) {
        say("Não consegui abrir a sua conta agora. Nenhum dado foi guardado.");
        oferta_humano(b, "falha ao abrir conta no cadastro");
        return;
    }
    b->conta = c;
    fmt_valor(c->saldo, s, sizeof s);
    say("Pronto, %s! Sua conta foi aberta e a identificação já está confirmada.", primeiro_nome(c));
    emit("   Conta:  %s\n", c->numero);
    emit("   Saldo:  %s\n", s);
    sim("SMS para o celular cadastrado: \"Facility Bank: sua conta foi aberta. "
        "Se não foi você, fale com a gente por este chat.\"");
    menu_principal(b, 1);
}

static void h_cad_confirma(Bot *b, const char *n)
{
    int o = escolha(n, 3);
    if (o < 0 && (igual(n, "confirmar") || igual(n, "sim"))) o = 1;
    switch (o) {
    case 1: concluir_cadastro(b); break;
    case 2:
        say("Sem problema, vamos refazer o cadastro do começo.");
        limpa_cadastro(b);
        ir(b, S_CAD_NOME);
        break;
    case 3: cancela_cadastro(b); break;
    default: nao_entendi(b);
    }
}

/* ---------------------------------------------------- senhas por estado */

static void h_pins(Bot *b, const char *in)
{
    int r;
    switch (b->estado) {
    case S_TX_PIN:
        r = checa_pin(b, b->conta, in);
        if (r == 1) { b->tx.pin_ok = 1; proxima_verificacao(b); }
        else if (r < 0) falha_seguranca(b, "senha errada 3 vezes ao confirmar operação");
        break;
    case S_CARTAO_PIN:
        r = checa_pin(b, b->conta, in);
        if (r == 1) {
            b->conta->cartao_bloqueado = 0;
            say("Pronto, cartão desbloqueado.");
            ir(b, S_CARTAO);
        } else if (r < 0) falha_seguranca(b, "senha errada 3 vezes ao desbloquear cartão");
        break;
    case S_BEN_PIN:
        r = checa_pin(b, b->conta, in);
        if (r == 1) {
            int rc = banco_benef_add(b->conta, b->tmp_apelido, b->tmp_nome, b->tmp_chave);
            say(rc == 0 ? "Pronto, %s foi salvo na sua lista." : "Não consegui salvar agora. Tente de novo mais tarde.",
                b->tmp_apelido);
            ir(b, S_BEN_MENU);
        } else if (r < 0) falha_seguranca(b, "senha errada 3 vezes ao cadastrar beneficiário");
        break;
    case S_ACOMP_PIN:
        r = checa_pin(b, b->conta, in);
        if (r == 1) {
            Conta *c = b->conta;
            if (b->tmp_n) {
                char v[32];
                c->acomp_ativo = 1;
                snprintf(c->acomp_nome, sizeof c->acomp_nome, "%s", b->tmp_nome);
                snprintf(c->acomp_tel, sizeof c->acomp_tel, "%s", b->tmp_tel);
                c->acomp_limite = b->tmp_valor;
                fmt_valor(c->acomp_limite, v, sizeof v);
                say("Pronto! Modo acompanhante ligado. %s vai aprovar os pagamentos a partir de %s.", c->acomp_nome, v);
                sim("Mensagem para %s: \"%s indicou você como acompanhante no Facility Bank.\"", c->acomp_nome, c->nome);
            } else {
                c->acomp_ativo = 0;
                say("Pronto, modo acompanhante desligado.");
                sim("Mensagem para %s: \"%s desligou o modo acompanhante.\"", c->acomp_nome, c->nome);
            }
            ir(b, S_ACOMP_MENU);
        } else if (r < 0) falha_seguranca(b, "senha errada 3 vezes ao alterar modo acompanhante");
        break;
    default: break;
    }
}

/* ------------------------------------------------------------ API publica */

void bot_init(Bot *b)
{
    memset(b, 0, sizeof *b);
    b->estado = S_INICIO;
    b->tx.boleto_idx = -1;
}

/* Primeira mensagem: apresenta a Bia e leva ao login, ao cadastro, ou pergunta qual dos dois. */
static void bot_inicio(Bot *b, const char *n)
{
    say("Olá! Eu sou a Bia, assistente virtual do Facility Bank.\n"
        "(A qualquer momento você pode digitar VOLTAR, CANCELAR, MENU, ATENDENTE para falar com uma pessoa, ou SAIR.)");
    if (quer_cadastro(n))    inicia_cadastro(b);
    else if (ja_cliente(n))  inicia_login(b);
    else                     ir(b, S_ACESSO);
}

static int em_alerta(Estado e)
{
    return e == S_ALERTA_SUSP || e == S_ALERTA_SUSP_PIN || e == S_ALERTA_BOLETO ||
           e == S_ALERTA_INFO || e == S_OFERTA_HUMANO;
}

void bot_processar(Bot *b, const char *mensagem)
{
    char n[128];
    norm(mensagem, n, sizeof n);

    if (b->estado == S_INICIO) { bot_inicio(b, n); return; }
    if (b->estado == S_HUMANO) { h_humano(b, n); return; }

    if (!*n) {
        say("Não recebi a sua mensagem. Pode escrever de novo?");
        pergunta(b);
        return;
    }

    if (eh_humano(n)) { handoff(b, "pedido do cliente"); return; }
    if (igual(n, "sair") || igual(n, "tchau") || igual(n, "encerrar")) { fim_sessao(b); return; }

    if (!em_alerta(b->estado)) {
        if (igual(n, "cancelar")) {
            if (em_cadastro(b->estado)) {
                cancela_cadastro(b);
            } else if (b->estado == S_ACESSO || b->estado == S_CPF || b->estado == S_PIN) {
                say("Tudo bem, cancelei. Quando precisar, é só mandar um OI.");
                limpa_sessao(b);
                b->estado = S_INICIO;
            } else cancelar_op(b);
            return;
        }
        if (igual(n, "menu") || igual(n, "inicio")) {
            if (b->conta) menu_principal(b, 0); else pergunta(b);
            return;
        }
        if (igual(n, "voltar")) {
            Estado a = anterior(b);
            if (a == b->estado || a == S_MENU) {
                if (b->conta) { if (a == S_MENU) menu_principal(b, 0); else pergunta(b); }
                else pergunta(b);
            } else {
                if (a == S_TX_CONFIRMA) { b->tx.pin_ok = 0; b->tx.sms_ok = 0; }
                ir(b, a);
            }
            return;
        }
        if (igual(n, "ajuda")) {
            say("Estou aqui para ajudar. Você pode digitar:\n"
                "   VOLTAR - para o passo anterior\n"
                "   CANCELAR - para desistir do que está fazendo\n"
                "   MENU - para ver as opções\n"
                "   ATENDENTE - para falar com uma pessoa\n"
                "   SAIR - para encerrar");
            pergunta(b);
            return;
        }
    }

    g_incomp_anterior = b->incompreendidas;
    b->incompreendidas = 0;           /* so volta a contar se nao_entendi() for chamado */
    switch (b->estado) {
    case S_ACESSO:         h_acesso(b, n); break;
    case S_CPF:            h_cpf(b, mensagem); break;
    case S_PIN:            h_pin_login(b, mensagem); break;
    case S_CAD_NOME:       h_cad_nome(b, mensagem); break;
    case S_CAD_CPF:        h_cad_cpf(b, mensagem); break;
    case S_CAD_NASC:       h_cad_nasc(b, mensagem); break;
    case S_CAD_TEL:        h_cad_tel(b, mensagem); break;
    case S_CAD_SENHA:      h_cad_senha(b, mensagem); break;
    case S_CAD_SENHA2:     h_cad_senha2(b, mensagem); break;
    case S_CAD_CONFIRMA:   h_cad_confirma(b, n); break;
    case S_MENU:           h_menu(b, n); break;
    case S_MAIS:           h_mais(b, n); break;
    case S_EXTRATO:        h_extrato(b, n); break;
    case S_PIX_CHAVE:      h_pix_chave(b, mensagem, n); break;
    case S_TED_ESCOLHE:    h_ted_escolhe(b, n); break;
    case S_TX_VALOR:       h_valor(b, mensagem); break;
    case S_BOL_ESCOLHE:    h_bol_escolhe(b, n); break;
    case S_BOL_CODIGO:     h_bol_codigo(b, mensagem); break;
    case S_TX_CONFIRMA:    h_confirma(b, n); break;
    case S_TX_GOLPE:       h_golpe(b, n); break;
    case S_TX_ACOMP:       h_codigo(b, mensagem, COD_ACOMP, &b->tx.acomp_ok, "código do acompanhante errado 3 vezes"); break;
    case S_TX_SMS:         h_codigo(b, mensagem, COD_SMS, &b->tx.sms_ok, "código SMS errado 3 vezes"); break;
    case S_TX_PIN: case S_CARTAO_PIN: case S_BEN_PIN: case S_ACOMP_PIN:
                           h_pins(b, mensagem); break;
    case S_CARTAO:         h_cartao(b, n); break;
    case S_CARTAO_CONF:    h_cartao_conf(b, n); break;
    case S_BEN_MENU:       h_ben_menu(b, n); break;
    case S_BEN_CHAVE:      h_ben_chave(b, mensagem); break;
    case S_BEN_APELIDO:    h_ben_apelido(b, mensagem); break;
    case S_BEN_REMOVE:     h_ben_remove(b, n); break;
    case S_BEN_REMOVE_CONF:h_ben_remove_conf(b, n); break;
    case S_ACOMP_MENU:     h_acomp_menu(b, n); break;
    case S_ACOMP_NOME:     h_acomp_nome(b, mensagem); break;
    case S_ACOMP_TEL:      h_acomp_tel(b, mensagem); break;
    case S_ACOMP_LIMITE:   h_acomp_limite(b, mensagem); break;
    case S_ALERTA_SUSP:    h_alerta_susp(b, n); break;
    case S_ALERTA_SUSP_PIN:h_alerta_susp_pin(b, mensagem); break;
    case S_ALERTA_BOLETO:  h_alerta_boleto(b, n); break;
    case S_ALERTA_INFO:    h_alerta_info(b, n); break;
    case S_OFERTA_HUMANO:  h_oferta_humano(b, n); break;
    default: break;
    }
}

/* ------------------------------------------- o bot fala primeiro (eventos) */

int bot_evento_por_nome(const char *nome)
{
    static const char *nomes[EV_TOTAL] = { "suspeito", "boleto", "pix", "fatura" };
    int i;
    for (i = 0; i < EV_TOTAL; i++) if (strcmp(nome, nomes[i]) == 0) return i;
    return -1;
}

void bot_evento(Bot *b, Evento ev)
{
    Conta *c = banco_conta_demo();     /* conta dona do numero que recebe a notificacao */
    char v[32];
    b->alvo = c;
    b->incompreendidas = 0;
    b->tentativas = 0;

    switch (ev) {
    case EV_SUSPEITO:
        snprintf(b->alerta_nome, sizeof b->alerta_nome, "Carlos Mendes");
        b->alerta_valor = 198000;
        b->estado = S_ALERTA_SUSP;
        pergunta(b);
        break;
    case EV_BOLETO_VENCE:
        b->pend_idx = 0;
        fmt_valor(c->boletos[0].valor, v, sizeof v);
        say("Lembrete do Facility Bank: o boleto de %s, no valor de %s, vence amanhã (%s). Quer pagar agora?",
            c->boletos[0].cedente, v, c->boletos[0].vencimento);
        b->estado = S_ALERTA_BOLETO;
        pergunta(b);
        break;
    case EV_FATURA:
        b->pend_idx = 2;
        fmt_valor(c->cartao_fatura, v, sizeof v);
        say("Aviso do Facility Bank: a fatura do seu cartão final %s fechou em %s e vence em %s. Quer pagar agora?",
            c->cartao_final, v, c->cartao_venc);
        b->estado = S_ALERTA_BOLETO;
        pergunta(b);
        break;
    case EV_PIX_RECEBIDO:
        banco_lancar(c, "Pix recebido - Ana Beatriz", 20000);
        say("Aviso do Facility Bank: você recebeu um Pix de R$ 200,00 de Ana Beatriz Silva.");
        b->estado = S_ALERTA_INFO;
        pergunta(b);
        break;
    default: break;
    }
}

void bot_aviso_inatividade(Bot *b)
{
    if (b->estado == S_INICIO || b->estado == S_HUMANO) return;
    say("Você ainda está aí? Se eu não receber resposta em 1 minuto, vou encerrar a conversa por segurança. "
        "Digite qualquer coisa para continuar.");
}

void bot_inatividade(Bot *b)
{
    if (b->estado == S_INICIO || b->estado == S_HUMANO) return;
    say("Como faz um tempinho que não recebo mensagem, encerrei a nossa conversa para proteger a sua conta. "
        "Nada em andamento foi feito. Quando quiser continuar, é só mandar um OI.");
    limpa_sessao(b);
    b->estado = S_INICIO;
}

==============================================================================
=== ARQUIVO: src/main.c
==============================================================================
/* main.c - simulador de canal (console) para o chatbot bancario.
 *
 * Funciona como um "WhatsApp de mentira": o que voce digita e a mensagem do cliente
 * e tudo que comeca com "Bia:" e a resposta do bot.
 *
 * Uso:
 *   chatbot                      espera o cliente escrever primeiro
 *   chatbot --evento suspeito    o BOT fala primeiro (suspeito | boleto | pix | fatura)
 *   chatbot --inatividade 120    segundos sem mensagem ate encerrar a sessao
 *
 * Comandos do simulador (nao fazem parte da conversa):
 *   #evento <nome>   dispara uma notificacao do banco agora
 *   #aviso           simula o aviso de "voce ainda esta ai?"
 *   #inativo         simula o fim de sessao por inatividade
 *   #sair            fecha o simulador
 */
#include "bot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define E_TERMINAL() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define E_TERMINAL() isatty(0)
#endif

#define INATIVIDADE_PADRAO 180

static void uso(void)
{
    puts("Uso: chatbot [--evento suspeito|boleto|pix|fatura] [--inatividade SEGUNDOS]");
}

int main(int argc, char **argv)
{
    Bot bot;
    char linha[512];
    int inatividade = INATIVIDADE_PADRAO;
    int ev_inicial = -1, terminal, i;
    time_t ultimo;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--evento") == 0 && i + 1 < argc) {
            ev_inicial = bot_evento_por_nome(argv[++i]);
            if (ev_inicial < 0) { uso(); return 2; }
        } else if (strcmp(argv[i], "--inatividade") == 0 && i + 1 < argc) {
            inatividade = atoi(argv[++i]);
            if (inatividade <= 0) { uso(); return 2; }
        } else {
            uso();
            return 2;
        }
    }

    banco_init();
    bot_init(&bot);
    terminal = E_TERMINAL();

    puts("=== Facility Bank - simulador de atendimento (Bia) ===");
    puts("Conta de demonstração: CPF 529.982.247-25, senha 1234.");
    puts("Comandos do simulador: #evento <suspeito|boleto|pix|fatura>, #aviso, #inativo, #sair");

    if (ev_inicial >= 0) bot_evento(&bot, (Evento)ev_inicial);   /* o bot fala primeiro */

    ultimo = time(NULL);
    for (;;) {
        size_t len;
        printf("\nVocê> ");
        fflush(stdout);
        if (!fgets(linha, sizeof linha, stdin)) break;
        len = strlen(linha);
        while (len > 0 && (linha[len - 1] == '\n' || linha[len - 1] == '\r')) linha[--len] = '\0';
        if (strncmp(linha, "\xEF\xBB\xBF", 3) == 0) memmove(linha, linha + 3, strlen(linha + 3) + 1);  /* BOM */
        if (!terminal) printf("%s\n", linha);             /* deixa o roteiro legivel */

        if (strcmp(linha, "#sair") == 0) break;
        if (strcmp(linha, "#aviso") == 0)   { bot_aviso_inatividade(&bot); continue; }
        if (strcmp(linha, "#inativo") == 0) { bot_inatividade(&bot); continue; }
        if (strncmp(linha, "#evento ", 8) == 0) {
            int ev = bot_evento_por_nome(linha + 8);
            if (ev < 0) puts("(simulador) evento inválido: suspeito, boleto, pix ou fatura");
            else bot_evento(&bot, (Evento)ev);
            ultimo = time(NULL);
            continue;
        }

        if (difftime(time(NULL), ultimo) >= inatividade) bot_inatividade(&bot);
        ultimo = time(NULL);
        bot_processar(&bot, linha);
    }
    puts("\n(simulador encerrado)");
    return 0;
}

==============================================================================
=== ARQUIVO: src/server.c
==============================================================================
/* server.c - servidor HTTP local minimo (so em 127.0.0.1) que liga a tela web ao bot em C.
 *
 * TODA a logica do atendimento continua em C (bot.c / banco.c). Este arquivo so:
 *   - entrega web/index.html;
 *   - recebe a mensagem do cliente em POST /api/msg e devolve o texto que o bot escreveu;
 *   - POST /api/evento (bot fala primeiro) e POST /api/cmd (aviso | inativo | reset).
 *
 * Uso: facility_web [porta]    (padrao 8080)  ->  abrir http://localhost:8080
 */
#include "bot.h"
#ifdef USAR_SQL
#include "db.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET sock_t;
#define SOCK_INVALIDO INVALID_SOCKET
#define fecha_sock closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALIDO (-1)
#define fecha_sock close
#endif

#define REQ_MAX        8192
#define INATIVIDADE_S  180

static Bot    g_bot;
static time_t g_ultimo;
static char   g_saida[32768];

static void envia_tudo(sock_t c, const char *p, size_t n)
{
    while (n > 0) {
        int r = send(c, p, (int)(n > 65536 ? 65536 : n), 0);
        if (r <= 0) return;
        p += r;
        n -= (size_t)r;
    }
}

static void responde(sock_t c, int codigo, const char *tipo, const char *corpo, size_t n)
{
    char cab[256];
    const char *msg = codigo == 200 ? "OK" : codigo == 404 ? "Not Found" :
                      codigo == 403 ? "Forbidden" : codigo == 413 ? "Payload Too Large" : "Bad Request";
    int len = snprintf(cab, sizeof cab,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        codigo, msg, tipo, (unsigned long)n);
    envia_tudo(c, cab, (size_t)len);
    envia_tudo(c, corpo, n);
}

static void erro_http(sock_t c, int codigo, const char *txt)
{
    responde(c, codigo, "text/plain; charset=utf-8", txt, strlen(txt));
}

/* compara o inicio de s com p, ignorando maiusculas */
static int prefixo_ci(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return 0;
    return 1;
}

/* valor do cabecalho `nome:` (nome com os dois-pontos) ou NULL */
static const char *cabecalho(const char *req, const char *fim, const char *nome)
{
    const char *p = strstr(req, "\r\n");
    while (p && p < fim) {
        p += 2;
        if (prefixo_ci(p, nome)) {
            p += strlen(nome);
            while (*p == ' ') p++;
            return p;
        }
        p = strstr(p, "\r\n");
    }
    return NULL;
}

/* Aceita localhost e os enderecos temporarios do tunel Cloudflare (https://xxxx.trycloudflare.com).
 * O servidor continua escutando so em 127.0.0.1: quem chega de fora passa pelo tunel. */
static int host_permitido(const char *h)
{
    char nome[128];
    size_t i = 0, len;
    static const char sufixo[] = ".trycloudflare.com";
    if (!h) return 0;
    while (h[i] && h[i] != ':' && h[i] != '\r' && h[i] != '\n' && i < sizeof nome - 1) {
        nome[i] = (char)tolower((unsigned char)h[i]);
        i++;
    }
    nome[i] = '\0';
    if (strcmp(nome, "localhost") == 0 || strcmp(nome, "127.0.0.1") == 0) return 1;
    len = strlen(nome);
    return len > sizeof sufixo - 1 && strcmp(nome + len - (sizeof sufixo - 1), sufixo) == 0;
}

static void serve_arquivo(sock_t c)
{
    static const char *caminhos[] = { "web/index.html", "../web/index.html", "chatbot-bancario/web/index.html" };
    size_t i;
    for (i = 0; i < sizeof caminhos / sizeof caminhos[0]; i++) {
        FILE *f = fopen(caminhos[i], "rb");
        long n;
        char *buf;
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (char *)malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            responde(c, 200, "text/html; charset=utf-8", buf, (size_t)n);
            free(buf);
            fclose(f);
            return;
        }
        free(buf);
        fclose(f);
    }
    erro_http(c, 404, "web/index.html nao encontrado (rode a partir da pasta chatbot-bancario)");
}

static void reinicia(void)
{
    banco_init();
    bot_init(&g_bot);
    g_ultimo = time(NULL);
}

static void atende(sock_t c)
{
    char req[REQ_MAX + 1];
    char metodo[8], caminho[128], *corpo;
    size_t n = 0;
    int fim_cab = -1;
    long clen = 0;
    const char *h;

    while (n < REQ_MAX) {
        int r = recv(c, req + n, (int)(REQ_MAX - n), 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = '\0';
        if (fim_cab < 0) {
            const char *p = strstr(req, "\r\n\r\n");
            if (p) {
                fim_cab = (int)(p - req) + 4;
                h = cabecalho(req, req + fim_cab, "content-length:");
                clen = h ? atol(h) : 0;
                if (clen < 0 || clen > REQ_MAX - fim_cab) { erro_http(c, 413, "mensagem grande demais"); return; }
            }
        }
        if (fim_cab >= 0 && (long)n >= fim_cab + clen) break;
    }
    if (fim_cab < 0 || (long)n < fim_cab + clen) { erro_http(c, 400, "requisicao incompleta"); return; }

    /* so aceita acesso pelo proprio computador (protege contra DNS rebinding) */
    h = cabecalho(req, req + fim_cab, "host:");
    if (!host_permitido(h)) {
        erro_http(c, 403, "host nao permitido");
        return;
    }

    if (sscanf(req, "%7s %127s", metodo, caminho) != 2) { erro_http(c, 400, "requisicao invalida"); return; }
    corpo = req + fim_cab;
    corpo[clen] = '\0';
    {   /* tira quebras de linha e limita o tamanho */
        size_t l = strlen(corpo);
        while (l > 0 && (corpo[l - 1] == '\n' || corpo[l - 1] == '\r')) corpo[--l] = '\0';
        if (l > 500) corpo[500] = '\0';
    }

    if (strcmp(metodo, "GET") == 0 && (strcmp(caminho, "/") == 0 || strcmp(caminho, "/index.html") == 0)) {
        serve_arquivo(c);
        return;
    }
    if (strcmp(metodo, "POST") != 0) { erro_http(c, 404, "nao encontrado"); return; }

    bot_saida_buffer(g_saida, sizeof g_saida);
    if (strcmp(caminho, "/api/msg") == 0) {
        if (difftime(time(NULL), g_ultimo) >= INATIVIDADE_S) bot_inatividade(&g_bot);
        g_ultimo = time(NULL);
        bot_processar(&g_bot, corpo);
    } else if (strcmp(caminho, "/api/evento") == 0) {
        int ev = bot_evento_por_nome(corpo);
        if (ev < 0) { bot_saida_buffer(NULL, 0); erro_http(c, 400, "evento invalido"); return; }
        g_ultimo = time(NULL);
        bot_evento(&g_bot, (Evento)ev);
    } else if (strcmp(caminho, "/api/cmd") == 0) {
        if (strcmp(corpo, "reset") == 0) reinicia();
        else if (strcmp(corpo, "aviso") == 0) bot_aviso_inatividade(&g_bot);
        else if (strcmp(corpo, "inativo") == 0) bot_inatividade(&g_bot);
        else { bot_saida_buffer(NULL, 0); erro_http(c, 400, "comando invalido"); return; }
    } else {
        bot_saida_buffer(NULL, 0);
        erro_http(c, 404, "nao encontrado");
        return;
    }
    bot_saida_buffer(NULL, 0);
    responde(c, 200, "text/plain; charset=utf-8", g_saida, strlen(g_saida));
}

int main(int argc, char **argv)
{
    int porta = argc > 1 ? atoi(argv[1]) : 8080;
    struct sockaddr_in a;
    sock_t s;

    if (porta <= 0 || porta > 65535) { fprintf(stderr, "porta invalida\n"); return 2; }
#ifdef _WIN32
    {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { fprintf(stderr, "falha ao iniciar winsock\n"); return 1; }
    }
#endif
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALIDO) { fprintf(stderr, "falha ao criar socket\n"); return 1; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)porta);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);          /* somente 127.0.0.1 */
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
        fprintf(stderr, "nao consegui usar a porta %d (ja esta em uso?). Tente: facility_web 8081\n", porta);
        return 1;
    }

#ifdef USAR_SQL
    db_iniciar();     /* sem banco.conf / FB_DB_CONN segue em memoria; se falhar, tenta de novo a cada uso */
#endif
    reinicia();
    printf("Facility Bank - atendimento web\nAbra no navegador: http://localhost:%d\n(Ctrl+C para parar)\n", porta);
    fflush(stdout);
    for (;;) {
        sock_t c = accept(s, NULL, NULL);
        if (c == SOCK_INVALIDO) continue;
        atende(c);
        fecha_sock(c);
    }
}

==============================================================================
=== ARQUIVO: src/wasm_api.c
==============================================================================
/* wasm_api.c - porta de entrada do bot (em C) quando compilado para WebAssembly.
 * O navegador (web/loader.js) chama estas funcoes; toda a logica continua em bot.c/banco.c.
 *
 * Fluxo: JS escreve o texto do cliente em fb_entrada(), chama fb_msg() e le o texto devolvido. */
#include "bot.h"

#include <string.h>
#include <time.h>

#define INATIVIDADE_S 180
#define EXPORTA(nome) __attribute__((export_name(nome)))

static Bot    g_bot;
static time_t g_ultimo;
static char   g_saida[32768];
static char   g_entrada[512];

static void apara_entrada(void)
{
    size_t l;
    g_entrada[sizeof g_entrada - 1] = '\0';
    l = strlen(g_entrada);
    while (l > 0 && (g_entrada[l - 1] == '\n' || g_entrada[l - 1] == '\r')) g_entrada[--l] = '\0';
}

EXPORTA("fb_entrada") char *fb_entrada(void) { return g_entrada; }
EXPORTA("fb_tam_entrada") int fb_tam_entrada(void) { return (int)sizeof g_entrada; }

EXPORTA("fb_init") void fb_init(void)
{
    banco_init();
    bot_init(&g_bot);
    g_ultimo = time(NULL);
}

EXPORTA("fb_msg") const char *fb_msg(void)
{
    apara_entrada();
    bot_saida_buffer(g_saida, sizeof g_saida);
    if (difftime(time(NULL), g_ultimo) >= INATIVIDADE_S) bot_inatividade(&g_bot);
    g_ultimo = time(NULL);
    bot_processar(&g_bot, g_entrada);
    bot_saida_buffer(NULL, 0);
    return g_saida;
}

/* NULL (0) = evento invalido */
EXPORTA("fb_evento") const char *fb_evento(void)
{
    int ev;
    apara_entrada();
    ev = bot_evento_por_nome(g_entrada);
    if (ev < 0) return NULL;
    bot_saida_buffer(g_saida, sizeof g_saida);
    g_ultimo = time(NULL);
    bot_evento(&g_bot, (Evento)ev);
    bot_saida_buffer(NULL, 0);
    return g_saida;
}

/* "reset" | "aviso" | "inativo"; NULL (0) = comando invalido */
EXPORTA("fb_cmd") const char *fb_cmd(void)
{
    apara_entrada();
    bot_saida_buffer(g_saida, sizeof g_saida);
    if (strcmp(g_entrada, "reset") == 0) fb_init();
    else if (strcmp(g_entrada, "aviso") == 0) bot_aviso_inatividade(&g_bot);
    else if (strcmp(g_entrada, "inativo") == 0) bot_inatividade(&g_bot);
    else { bot_saida_buffer(NULL, 0); return NULL; }
    bot_saida_buffer(NULL, 0);
    return g_saida;
}

==============================================================================
=== ARQUIVO: banco/FaciliteBank.sql
==============================================================================
/* FaciliteBank.sql - cria o banco do chatbot (SQL Server) conforme o DER.
 *
 * Rode no SSMS (ou sqlcmd) com um usuario que possa criar banco/tabelas.
 * E seguro rodar mais de uma vez: so cria o que ainda nao existe.
 *
 * ATENCAO: se voce ja rodou o script antigo (CPF INT, DECIMAL sem casas, etc.), as tabelas
 * antigas continuam la e este script NAO as corrige. Nesse caso, descomente o bloco
 * "RECOMECAR DO ZERO" abaixo (apaga TODOS os dados dessas tabelas) e rode de novo.
 *
 * O chatbot usa hoje CLIENTE, CONTA e a sequencia SEQ_NUMERO_CONTA. As demais tabelas ja
 * existem para as proximas etapas (Pix, boleto, cartao, acompanhante...).
 */
IF DB_ID('FaciliteBank') IS NULL CREATE DATABASE FaciliteBank;
GO
USE FaciliteBank;
GO

/* ---------------------------------------------------------- RECOMECAR DO ZERO
DROP TABLE IF EXISTS dbo.PERMISSAO_ACOMPANHANTE;
DROP TABLE IF EXISTS dbo.TRANSACAO_PIX;
DROP TABLE IF EXISTS dbo.ALERTA;
DROP TABLE IF EXISTS dbo.BOLETO;
DROP TABLE IF EXISTS dbo.CARTAO;
DROP TABLE IF EXISTS dbo.SOLICITACAO_ATENDIMENTO;
DROP TABLE IF EXISTS dbo.ACOMPANHANTE;
DROP TABLE IF EXISTS dbo.BENEFICIARIO;
DROP TABLE IF EXISTS dbo.CONTA;
DROP TABLE IF EXISTS dbo.CLIENTE;
DROP SEQUENCE IF EXISTS dbo.SEQ_NUMERO_CONTA;
GO
---------------------------------------------------------------------------- */

/* Numero das contas novas (o chatbot busca o proximo valor e monta "100001-1"). */
IF OBJECT_ID('dbo.SEQ_NUMERO_CONTA', 'SO') IS NULL
    CREATE SEQUENCE dbo.SEQ_NUMERO_CONTA AS BIGINT START WITH 100001 INCREMENT BY 1;
GO

/* A senha NUNCA fica em texto: guarda-se um "sal" aleatorio e o SHA-256 de (sal + senha). */
IF OBJECT_ID('dbo.CLIENTE', 'U') IS NULL
CREATE TABLE dbo.CLIENTE (
    id_cliente      INT IDENTITY(1,1) CONSTRAINT PK_CLIENTE PRIMARY KEY,
    nome            NVARCHAR(100) NOT NULL,
    cpf             CHAR(11)      NOT NULL CONSTRAINT UQ_CLIENTE_cpf UNIQUE,
    data_nascimento DATE          NOT NULL,
    telefone        VARCHAR(11)   NOT NULL,
    senha_salt      CHAR(32)      NOT NULL,
    senha_hash      CHAR(64)      NOT NULL
);
GO

/* "possui" e 1:1 no DER: o UNIQUE em id_cliente impede mais de uma conta por cliente. */
IF OBJECT_ID('dbo.CONTA', 'U') IS NULL
CREATE TABLE dbo.CONTA (
    id_conta      INT IDENTITY(1,1) CONSTRAINT PK_CONTA PRIMARY KEY,
    id_cliente    INT           NOT NULL CONSTRAINT UQ_CONTA_cliente UNIQUE,
    numero        VARCHAR(20)   NOT NULL CONSTRAINT UQ_CONTA_numero UNIQUE,
    saldo         DECIMAL(15,2) NOT NULL CONSTRAINT DF_CONTA_saldo DEFAULT 0,
    data_abertura DATE          NOT NULL CONSTRAINT DF_CONTA_abertura DEFAULT (CAST(GETDATE() AS DATE)),
    CONSTRAINT FK_CONTA_CLIENTE FOREIGN KEY (id_cliente) REFERENCES dbo.CLIENTE (id_cliente)
);
GO

IF OBJECT_ID('dbo.BENEFICIARIO', 'U') IS NULL
CREATE TABLE dbo.BENEFICIARIO (
    id_beneficiario INT IDENTITY(1,1) CONSTRAINT PK_BENEFICIARIO PRIMARY KEY,
    id_cliente      INT           NOT NULL,
    nome            NVARCHAR(100) NOT NULL,
    chave_pix       VARCHAR(255)  NOT NULL,
    CONSTRAINT FK_BENEFICIARIO_CLIENTE FOREIGN KEY (id_cliente) REFERENCES dbo.CLIENTE (id_cliente)
);
GO

IF OBJECT_ID('dbo.ACOMPANHANTE', 'U') IS NULL
CREATE TABLE dbo.ACOMPANHANTE (
    id_acompanhante INT IDENTITY(1,1) CONSTRAINT PK_ACOMPANHANTE PRIMARY KEY,
    nome            NVARCHAR(100) NOT NULL,
    cpf             CHAR(11)      NOT NULL CONSTRAINT UQ_ACOMPANHANTE_cpf UNIQUE
);
GO

IF OBJECT_ID('dbo.PERMISSAO_ACOMPANHANTE', 'U') IS NULL
CREATE TABLE dbo.PERMISSAO_ACOMPANHANTE (
    id_permissao    INT IDENTITY(1,1) CONSTRAINT PK_PERMISSAO PRIMARY KEY,
    id_cliente      INT         NOT NULL,
    id_acompanhante INT         NOT NULL,
    tipo_permissao  VARCHAR(50) NOT NULL,
    data_concessao  DATE        NOT NULL,
    status          BIT         NOT NULL,
    CONSTRAINT FK_PERMISSAO_CLIENTE      FOREIGN KEY (id_cliente)      REFERENCES dbo.CLIENTE (id_cliente),
    CONSTRAINT FK_PERMISSAO_ACOMPANHANTE FOREIGN KEY (id_acompanhante) REFERENCES dbo.ACOMPANHANTE (id_acompanhante)
);
GO

IF OBJECT_ID('dbo.SOLICITACAO_ATENDIMENTO', 'U') IS NULL
CREATE TABLE dbo.SOLICITACAO_ATENDIMENTO (
    id_solicitacao INT IDENTITY(1,1) CONSTRAINT PK_SOLICITACAO PRIMARY KEY,
    id_cliente     INT           NOT NULL,
    assunto        NVARCHAR(255) NOT NULL,
    status         BIT           NOT NULL,
    data_abertura  DATE          NOT NULL,
    CONSTRAINT FK_SOLICITACAO_CLIENTE FOREIGN KEY (id_cliente) REFERENCES dbo.CLIENTE (id_cliente)
);
GO

IF OBJECT_ID('dbo.CARTAO', 'U') IS NULL
CREATE TABLE dbo.CARTAO (
    id_cartao INT IDENTITY(1,1) CONSTRAINT PK_CARTAO PRIMARY KEY,
    id_conta  INT          NOT NULL,
    numero    VARCHAR(16)  NOT NULL,   /* em producao: guardar so os 4 ultimos digitos ou um token */
    tipo      VARCHAR(100) NOT NULL,
    status    BIT          NOT NULL,
    CONSTRAINT FK_CARTAO_CONTA FOREIGN KEY (id_conta) REFERENCES dbo.CONTA (id_conta)
);
GO

IF OBJECT_ID('dbo.BOLETO', 'U') IS NULL
CREATE TABLE dbo.BOLETO (
    id_boleto     INT IDENTITY(1,1) CONSTRAINT PK_BOLETO PRIMARY KEY,
    id_conta      INT           NOT NULL,
    codigo_barras VARCHAR(255)  NOT NULL,
    valor         DECIMAL(15,2) NOT NULL,
    vencimento    DATE          NOT NULL,
    status        BIT           NOT NULL,
    CONSTRAINT FK_BOLETO_CONTA FOREIGN KEY (id_conta) REFERENCES dbo.CONTA (id_conta)
);
GO

IF OBJECT_ID('dbo.ALERTA', 'U') IS NULL
CREATE TABLE dbo.ALERTA (
    id_alerta INT IDENTITY(1,1) CONSTRAINT PK_ALERTA PRIMARY KEY,
    id_conta  INT           NOT NULL,
    tipo      VARCHAR(50)   NOT NULL,
    descricao NVARCHAR(255) NOT NULL,
    data_hora DATETIME2     NOT NULL,
    CONSTRAINT FK_ALERTA_CONTA FOREIGN KEY (id_conta) REFERENCES dbo.CONTA (id_conta)
);
GO

IF OBJECT_ID('dbo.TRANSACAO_PIX', 'U') IS NULL
CREATE TABLE dbo.TRANSACAO_PIX (
    id_transacao    INT IDENTITY(1,1) CONSTRAINT PK_TRANSACAO_PIX PRIMARY KEY,
    id_conta        INT           NOT NULL,
    id_beneficiario INT           NOT NULL,
    valor           DECIMAL(15,2) NOT NULL,
    data_hora       DATETIME2     NOT NULL,
    status          BIT           NOT NULL,
    CONSTRAINT FK_PIX_CONTA        FOREIGN KEY (id_conta)        REFERENCES dbo.CONTA (id_conta),
    CONSTRAINT FK_PIX_BENEFICIARIO FOREIGN KEY (id_beneficiario) REFERENCES dbo.BENEFICIARIO (id_beneficiario)
);
GO

/* Usuario so para o chatbot (menor privilegio). Troque a senha antes de rodar!
 * Descomente e rode como administrador; depois use este usuario no banco.conf.
 *
 * CREATE LOGIN chatbot_facilite WITH PASSWORD = 'TROQUE-ESTA-SENHA-FORTE', CHECK_POLICY = ON;
 * CREATE USER  chatbot_facilite FOR LOGIN chatbot_facilite;
 * GRANT SELECT, INSERT ON dbo.CLIENTE TO chatbot_facilite;
 * GRANT SELECT, INSERT ON dbo.CONTA   TO chatbot_facilite;
 * GRANT UPDATE ON OBJECT::dbo.SEQ_NUMERO_CONTA TO chatbot_facilite;
 */

==============================================================================
=== ARQUIVO: banco.conf.exemplo
==============================================================================
# Copie este arquivo para "banco.conf" (na mesma pasta do facility_web.exe), apague os
# exemplos que nao for usar e preencha a linha de conexao. A primeira linha que nao
# comeca com # e usada. NUNCA envie o banco.conf para ninguem: ele tem a senha do banco.
#
# --- Login do SQL Server (usuario e senha). Recomendado: usuario so do chatbot (veja FaciliteBank.sql)
# Driver={ODBC Driver 17 for SQL Server};Server=NOME-OU-IP-DO-OUTRO-PC\SQLEXPRESS;Database=FaciliteBank;UID=chatbot_facilite;PWD=COLOQUE-A-SENHA;
#
# --- Se o servidor for acessado por porta (sem nome de instancia):
# Driver={ODBC Driver 17 for SQL Server};Server=192.168.0.10,1433;Database=FaciliteBank;UID=chatbot_facilite;PWD=COLOQUE-A-SENHA;
#
# --- Driver 18 (exige criptografia; num servidor de teste sem certificado valido, acrescente TrustServerCertificate=yes;)
# Driver={ODBC Driver 18 for SQL Server};Server=NOME-OU-IP;Database=FaciliteBank;UID=chatbot_facilite;PWD=COLOQUE-A-SENHA;TrustServerCertificate=yes;
#
# --- Autenticacao do Windows (a conta que abre o programa precisa ter acesso ao banco)
# Driver={ODBC Driver 17 for SQL Server};Server=NOME-OU-IP\SQLEXPRESS;Database=FaciliteBank;Trusted_Connection=Yes;
#
# Alternativa sem arquivo: defina a variavel de ambiente FB_DB_CONN com a mesma linha.

==============================================================================
=== ARQUIVO: Makefile
==============================================================================
CC     ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
SRC     = src/main.c src/bot.c src/banco.c

chatbot: $(SRC) src/bot.h src/banco.h
	$(CC) $(CFLAGS) -o chatbot $(SRC)

# roda os roteiros de validacao (ok, erro, golpe, cadastro) + os eventos em que o bot fala primeiro
teste: chatbot
	./chatbot < roteiros/ok.txt
	./chatbot < roteiros/erro.txt
	./chatbot < roteiros/golpe.txt
	./chatbot < roteiros/cadastro.txt
	./chatbot < roteiros/cadastro_erro.txt
	./chatbot --evento suspeito < roteiros/alerta_suspeito.txt
	./chatbot --evento boleto < roteiros/alerta_boleto.txt

clean:
	rm -f chatbot chatbot.exe

==============================================================================
=== ARQUIVO: build.bat
==============================================================================
@echo off
rem Compila com MinGW/gcc ou, se nao houver, com o MSVC (rode no "Developer Command Prompt").
where gcc >nul 2>nul
if %errorlevel%==0 (
  gcc -std=c99 -Wall -Wextra -O2 -o chatbot.exe src\main.c src\bot.c src\banco.c
) else (
  cl /nologo /W3 /utf-8 /Fe:chatbot.exe src\main.c src\bot.c src\banco.c
)

==============================================================================
=== ARQUIVO: iniciar_web.bat
==============================================================================
@echo off
rem Compila (se houver gcc) e abre o atendimento web em http://localhost:8080
cd /d "%~dp0"
where gcc >nul 2>nul
if %errorlevel%==0 gcc -std=c99 -Wall -Wextra -O2 -DUSAR_SQL -o facility_web.exe src\server.c src\bot.c src\banco.c src\db.c -lws2_32 -lodbc32
if not exist facility_web.exe (
  echo facility_web.exe nao existe e nao ha gcc para compilar.
  pause
  exit /b 1
)
start "" http://localhost:8080
facility_web.exe 8080

==============================================================================
=== ARQUIVO: build_standalone.ps1
==============================================================================
# Gera FacilityBank.html: UM arquivo so, com o bot em C (compilado para WebAssembly) dentro.
# Precisa do Zig (winget install zig.zig). Uso:  powershell -File build_standalone.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
zig cc -target wasm32-wasi -mexec-model=reactor -Oz -s -std=c99 -Wall -Wextra `
    -o fb.wasm src/wasm_api.c src/bot.c src/banco.c
if ($LASTEXITCODE -ne 0) { throw 'falha ao compilar para WebAssembly' }

$b64    = [Convert]::ToBase64String([IO.File]::ReadAllBytes("$PSScriptRoot\fb.wasm"))
$loader = [IO.File]::ReadAllText("$PSScriptRoot\web\loader.js", [Text.Encoding]::UTF8).Replace('__WASM_B64__', $b64)
$html   = [IO.File]::ReadAllText("$PSScriptRoot\web\index.html", [Text.Encoding]::UTF8)
$html   = $html.Replace("<script>`n", "<script>`n$loader`n</script>`n<script>`n")
if ($html -notmatch 'fbLocal') { $html = $html.Replace("<script>`r`n", "<script>`r`n$loader`r`n</script>`r`n<script>`r`n") }
[IO.File]::WriteAllText("$PSScriptRoot\FacilityBank.html", $html, (New-Object Text.UTF8Encoding $false))
Remove-Item fb.wasm
"FacilityBank.html gerado: {0:N0} KB" -f ((Get-Item "$PSScriptRoot\FacilityBank.html").Length / 1KB)

==============================================================================
=== ARQUIVO: roteiros/ok.txt
==============================================================================
oi
ja sou cliente
52998224725
1234
asdf
3
1
150
1
1234
sair
#sair

==============================================================================
=== ARQUIVO: roteiros/erro.txt
==============================================================================
oi
ja sou cliente
52998224725
0000
1234
3
maria@naoexiste.com
11144477735
3000
cancelar
sair
#sair

==============================================================================
=== ARQUIVO: roteiros/golpe.txt
==============================================================================
oi
ja sou cliente
52998224725
1234
3
novo.contato@mail-premio.com
1800
1
1
1
preciso de ajuda com isso
voltar ao bot
sair
#sair

==============================================================================
=== ARQUIVO: roteiros/cadastro.txt
==============================================================================
oi
2
Maria
Joana Pereira Souza
12345678900
12345678909
31/02/1950
25/03/1950
11 98765-4321
1234
2503
7391
7392
7391
7391
1
5
2
2
1
sair
#sair

==============================================================================
=== ARQUIVO: roteiros/cadastro_erro.txt
==============================================================================
quero me cadastrar
Joana Pereira Souza
52998224725
1234
sair
oi
2
voltar
cancelar
oi
2
Ana Lima
12345678909
15/08/2015
2
#sair

==============================================================================
=== ARQUIVO: roteiros/alerta_suspeito.txt
==============================================================================
2
2
#sair

==============================================================================
=== ARQUIVO: roteiros/alerta_boleto.txt
==============================================================================
1
52998224725
1234
1
1234
sair
#sair
