#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>

#define ALPHABET 26
#define MAX_WORD 100
#define MAX_INPUT 1000
#define MAX_SUGGESTIONS 5

typedef struct Word {
    char word[MAX_WORD];
    float frequency;
} Word;

typedef struct TrieNode {
    struct TrieNode *child[ALPHABET];
    Word *data;          /* NULL if no word ends here */
} TrieNode;

/* ---------- Trie ---------- */

TrieNode *createNode()
{
    TrieNode *node = (TrieNode *)malloc(sizeof(TrieNode));
    if (node == NULL) {
        printf("Memory allocation failed!\n");
        exit(1);
    }
    node->data = NULL;
    for (int i = 0; i < ALPHABET; i++)
        node->child[i] = NULL;
    return node;
}

/* Returns 1 if inserted, 0 if word has a non-letter and was skipped */
int insert(TrieNode *root, const char *word, float frequency)
{
    for (int i = 0; word[i] != '\0'; i++)
        if (!isalpha((unsigned char)word[i]))
            return 0;

    TrieNode *current = root;
    for (int i = 0; word[i] != '\0'; i++) {
        int index = tolower((unsigned char)word[i]) - 'a';
        if (current->child[index] == NULL)
            current->child[index] = createNode();
        current = current->child[index];
    }

    if (current->data == NULL) {
        current->data = (Word *)malloc(sizeof(Word));
        if (current->data == NULL) {
            printf("Memory allocation failed!\n");
            exit(1);
        }
    }
    strcpy(current->data->word, word);
    current->data->frequency = frequency;
    return 1;
}

void loadDictionary(TrieNode *root, const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        printf("ERROR: Cannot open %s\n", filename);
        exit(1);
    }

    char word[MAX_WORD];
    float frequency;
    long count = 0;

    while (fscanf(file, "%99s %f", word, &frequency) == 2)
        if (insert(root, word, frequency))
            count++;

    fclose(file);
    printf("Dictionary loaded: %ld words\n", count);
}

/* Walk down the trie following the prefix. NULL if prefix not found. */
TrieNode *findPrefix(TrieNode *root, const char *prefix)
{
    TrieNode *current = root;
    for (int i = 0; prefix[i] != '\0'; i++) {
        int index = tolower((unsigned char)prefix[i]) - 'a';
        if (index < 0 || index >= ALPHABET || current->child[index] == NULL)
            return NULL;
        current = current->child[index];
    }
    return current;
}

/* ---------- Suggestions (top 10 by frequency) ---------- */

void addSuggestion(Word *suggestions[], int *count, Word *word)
{
    if (*count < MAX_SUGGESTIONS) {
        suggestions[*count] = word;
        (*count)++;
    }
    else {
        if (word->frequency <= suggestions[MAX_SUGGESTIONS - 1]->frequency)
            return;
        suggestions[MAX_SUGGESTIONS - 1] = word;   /* replace the weakest */
    }

    /* bubble the new word up to its sorted position */
    for (int i = *count - 1; i > 0; i--) {
        if (suggestions[i]->frequency > suggestions[i - 1]->frequency) {
            Word *temp = suggestions[i];
            suggestions[i] = suggestions[i - 1];
            suggestions[i - 1] = temp;
        }
        else
            break;
    }
}

void collectSuggestions(TrieNode *node, Word *suggestions[], int *count)
{
    if (node == NULL)
        return;

    if (node->data != NULL)
        addSuggestion(suggestions, count, node->data);

    for (int i = 0; i < ALPHABET; i++)
        collectSuggestions(node->child[i], suggestions, count);
}

/* Copy the last word of the sentence into lastWord */
void getLastWord(const char *input, char *lastWord)
{
    int start = strlen(input);
    while (start > 0 && input[start - 1] != ' ')
        start--;
    strcpy(lastWord, input + start);
}

int updateSuggestions(TrieNode *root, const char *input, Word *suggestions[])
{
    int length = strlen(input);

    /* nothing typed, or a space was just typed -> no suggestions */
    if (length == 0 || input[length - 1] == ' ')
        return 0;

    char lastWord[MAX_WORD];
    getLastWord(input, lastWord);

    TrieNode *node = findPrefix(root, lastWord);
    if (node == NULL)
        return 0;

    int count = 0;
    collectSuggestions(node, suggestions, &count);
    return count;
}

/* ---------- Display ---------- */

void display(const char *input, Word *suggestions[], int count)
{
    system("cls");

    printf("========================================\n");
    printf("          C AUTO COMPLETE\n");
    printf("========================================\n\n");
    printf("Search: %s\n\n", input);

    for (int i = 0; i < count; i++)
        printf("    %-25s %.2f\n", suggestions[i]->word, suggestions[i]->frequency);

    printf("\n\nType to search   BACKSPACE = delete   ESC = exit\n");
}

/* ---------- Main loop ---------- */

void autocomplete(TrieNode *root)
{
    char input[MAX_INPUT] = "";
    int length = 0;
    int count = 0;
    Word *suggestions[MAX_SUGGESTIONS];

    display(input, suggestions, count);

    while (1) {
        int key = _getch();

        /* ESC */
        if (key == 27) {
            printf("\nExited.\n");
            break;
        }

        /* Arrow / function keys send 2 codes. Read and ignore the 2nd. */
        if (key == 0 || key == 224) {
            _getch();
            continue;
        }

        /* BACKSPACE */
        if (key == 8) {
            if (length > 0) {
                length--;
                input[length] = '\0';
            }
        }
        /* SPACE or LETTER */
        else if ((key == ' ' || isalpha(key)) && length < MAX_INPUT - 1) {
            input[length] = tolower(key);
            length++;
            input[length] = '\0';
        }

        count = updateSuggestions(root, input, suggestions);
        display(input, suggestions, count);
    }
}

int main()
{
    TrieNode *root = createNode();

    loadDictionary(root, "dictionary.txt");
    printf("\nPress any key to start...");
    _getch();

    autocomplete(root);
    return 0;
}