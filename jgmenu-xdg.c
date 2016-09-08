/*
 * jgmenu-xdg.c
 *
 * Parses XML .menu file and outputs a csv formatted jgmenu file
 *
 * This is still completely experimental and only just works
 *
 * It aim to be XDG compliant, although it has a long way to go!
 *
 * See this spec for further details:
 * https://specifications.freedesktop.org/menu-spec/menu-spec-1.0.html
 */

#include <stdio.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "xdgapps.h"
#include "util.h"
#include "sbuf.h"

#define DEBUG_PRINT_XML_NODES 0

static const char jgmenu_xdg_usage[] =
"Usage: jgmenu-xdg [options] <file>\n"
"    --data-dir=<dir>      specify directory containing .menu file\n";

/*
 * In jgmenu-speak:
 *	- A "node" is a root-menu or a submenu.
 *	  It corresponds roughly to an XML <Menu> element.
 *	- An "item" is a individual menu entry (with name, command and icon)
 *	  These are defined by "categories", .desktop-files, etc
 */
struct jgmenu_node {
	char *name;
	char *tag;
	char *directory;
	int level;
	struct jgmenu_node *parent;
	struct list_head category_includes;
	struct list_head category_excludes;
	struct list_head categories;
	struct list_head desktop_files;
	struct list_head menu_items;
	struct list_head list;
};

struct jgmenu_item {
	char *name;
	char *cmd;
	char *icon_name;
	struct list_head list;
};

static LIST_HEAD(jgmenu_nodes);
static struct jgmenu_node *current_jgmenu_node;

/* command line args */
static char *data_dir;

static void usage(void)
{
	printf("%s", jgmenu_xdg_usage);
	exit(0);
}

static void print_menu_item(struct jgmenu_item *item)
{
	printf("%s,", item->name);
	if (item->cmd)
		printf("%s", item->cmd);
	if (item->icon_name)
		printf(",%s", item->icon_name);
	printf("\n");
}

static void print_csv_menu(void)
{
	struct jgmenu_node *n;
	struct jgmenu_item *item;

	list_for_each_entry(n, &jgmenu_nodes, list) {
		if (list_empty(&n->menu_items))
			continue;

		printf("%s,^tag(%s)\n", n->name, n->tag);
		if (n->parent)
			printf("Go back,^checkout(%s),folder\n",
			       n->parent->name);

		/* Print directories first */
		list_for_each_entry(item, &n->menu_items, list)
			if (item->cmd && !strncmp(item->cmd, "^checkout", 9))
				print_menu_item(item);

		/* Then all other items */
		list_for_each_entry(item, &n->menu_items, list)
			if (!item->cmd || strncmp(item->cmd, "^checkout", 9))
				print_menu_item(item);

		printf("\n");
	}
}

static void init_jgmenu_item(struct jgmenu_item *item)
{
	item->name = NULL;
	item->cmd = NULL;
	item->icon_name = NULL;
}

static void add_item_to_jgmenu_node(struct jgmenu_node *node,
				    struct desktop_file_data *data)
{
	struct jgmenu_item *item;

	item = xmalloc(sizeof(struct jgmenu_item));
	init_jgmenu_item(item);
	if (data->name)
		item->name = strdup(data->name);
	if (data->exec)
		item->cmd = strdup(strstrip(data->exec));
	if (data->icon)
		item->icon_name = strdup(data->icon);
	list_add_tail(&item->list, &node->menu_items);
}

static void add_dir_to_jgmenu_node(struct jgmenu_node *parent,
				   const char *name,
				   const char *tag)
{
	struct jgmenu_item *item;
	struct sbuf s;

	if (!name || !tag)
		die("no name or tag specified in add_dir_to_jgmenu_node()");

	if (!parent)
		return;

	sbuf_init(&s);
	sbuf_addstr(&s, "^checkout(");
	sbuf_addstr(&s, tag);
	sbuf_addstr(&s, ")");

	item = xmalloc(sizeof(struct jgmenu_item));
	init_jgmenu_item(item);
	item->name = strdup(name);
	item->cmd = strdup(s.buf);
	item->icon_name = strdup("folder");
	list_add_tail(&item->list, &parent->menu_items);
}

/* TODO: This needs to be changed to categories rather than category_includes */
static void process_categories_and_populate_desktop_files(void)
{
	struct jgmenu_node *jgmenu_node;
	struct desktop_file_data *desktop_file;
	struct sbuf *cat;

	list_for_each_entry(jgmenu_node, &jgmenu_nodes, list) {
		list_for_each_entry(cat, &jgmenu_node->category_includes, list) {
			xdgapps_filter_desktop_files_on_category(cat->buf);
			list_for_each_entry(desktop_file, &desktop_files_filtered, filtered_list)
				add_item_to_jgmenu_node(jgmenu_node, desktop_file);
		}
	}
}

/*
 * For each directory within a <Menu></Menu>, a menu item is added to the
 * _parent_ jgmenu node. This is slighly counter intuitive, but it's just
 * how it is.
 *
 * TODO: Throw error if .directory file does not exist
 */
static void process_directory_files(void)
{
	struct jgmenu_node *jgmenu_node;
	struct directory_file_data *directory_file;

	list_for_each_entry(jgmenu_node, &jgmenu_nodes, list) {
		if (list_empty(&jgmenu_node->menu_items))
			continue;

		list_for_each_entry(directory_file, &directory_files, list)
			if (!strcmp(directory_file->filename, jgmenu_node->directory))
				add_dir_to_jgmenu_node(jgmenu_node->parent, jgmenu_node->name, jgmenu_node->tag);
	}
}

static void add_data_to_jgmenu_node(const char *xml_node_name,
				    const char *content)
{
	struct jgmenu_node *jgmenu_node;
	struct sbuf *category;

	jgmenu_node = current_jgmenu_node;

	/* TODO: Lots to be added here */
	/* TODO: Check for tag duplicates */
	if (!strcasecmp(xml_node_name, "Name")) {
		jgmenu_node->name = strdup(content);
		jgmenu_node->tag = strdup(content);
	} else if (!strcasecmp(xml_node_name, "Directory")) {
		jgmenu_node->directory = strdup(content);
	} else if (!strcasecmp(xml_node_name, "Include.And.Category")) {
		category = xmalloc(sizeof(struct sbuf));
		sbuf_init(category);
		sbuf_addstr(category, content);
		list_add_tail(&category->list, &jgmenu_node->category_includes);
	}

	if (DEBUG_PRINT_XML_NODES)
		printf("%d-%s: %s\n", jgmenu_node->level, xml_node_name, content);
}

static void create_new_jgmenu_node(struct jgmenu_node *parent,
				   int level)
{
	struct jgmenu_node *node;

	if (DEBUG_PRINT_XML_NODES)
		printf("---\n");

	node = xcalloc(1, sizeof(struct jgmenu_node));
	node->name = NULL;
	node->tag = NULL;
	node->directory = NULL;
	node->level = level;

	if (parent)
		node->parent = parent;
	else
		node->parent = NULL;

	INIT_LIST_HEAD(&node->category_includes);
	INIT_LIST_HEAD(&node->category_excludes);
	INIT_LIST_HEAD(&node->categories);
	INIT_LIST_HEAD(&node->desktop_files);
	INIT_LIST_HEAD(&node->menu_items);
	list_add_tail(&node->list, &jgmenu_nodes);

	current_jgmenu_node = node;
}

static int level(xmlNode *node)
{
	int level = 0;

	for (;;) {
		node = node->parent;
		if (!node || !node->name)
			return level;

		if (!strcasecmp((char *)node->name, "Menu"))
			++level;
	}
}

/*
 * Gives the node name back to the parent "Menu" element.
 * For example, it would convert
 * <Include><And><Category></Category></And></Include> to
 * "Include.And.Category"
 */
static void get_full_node_name(struct sbuf *node_name, xmlNode *node)
{
	int ismenu;

	if (!strcmp((char *)node->name, "text")) {
		node = node->parent;
		if (!node || !node->name) {
			fprintf(stderr, "warning: node is root\n");
			return;
		}
	}

	ismenu = !strcmp((char *)node->name, "Menu");
	for (;;) {
		if (!ismenu)
			sbuf_prepend(node_name, (char *)node->name);

		node = node->parent;
		if (!node || !node->name)
			return;

		ismenu = !strcmp((char *)node->name, "Menu");
		if (!ismenu)
			sbuf_prepend(node_name, ".");
	}
}

static void process_node(xmlNode *node)
{
	struct sbuf node_name;

	/*
	 * Just filtering out a lot of rubbish here for the time being.
	 * We will need to check for <OnlyUnallocated/> etc.
	 */
	if (!node->content)
		return;

	sbuf_init(&node_name);

	get_full_node_name(&node_name, node);

	if (node_name.len && strlen(strstrip((char *)node->content)))
		add_data_to_jgmenu_node(node_name.buf, strstrip((char *)node->content));

	free(node_name.buf);
}

static void revert_to_parent(void)
{
	if (current_jgmenu_node && current_jgmenu_node->parent)
		current_jgmenu_node = current_jgmenu_node->parent;
}

/*
 * n->next refers to siblings
 * n->children is obviously the children
 */
static void xml_tree_walk(xmlNode *node)
{
	xmlNode *n;

	for (n = node; n; n = n->next) {
		if (!strcasecmp((char *)n->name, "Menu")) {
			create_new_jgmenu_node(current_jgmenu_node, level(n));
			xml_tree_walk(n->children);
			revert_to_parent();
			continue;
		}

		if (!strcasecmp((char *)n->name, "Comment"))
			continue;

		process_node(n);
		xml_tree_walk(n->children);
	}
}

static void parse_xml(const char *filename)
{
	xmlDoc *d = xmlReadFile(filename, NULL, 0);

	if (!d)
		die("error reading file '%s'\n", filename);

	xml_tree_walk(xmlDocGetRootElement(d));
	xmlFreeDoc(d);
	xmlCleanupParser();
}

int main(int argc, char **argv)
{
	char *file_name = NULL;
	int i;

	if (argc < 2)
		usage();

	LIBXML_TEST_VERSION

	/* Create lists of .desktop- and .directory files */
	xdgapps_init_lists();

	i = 1;
	while (i < argc) {
		if (argv[i][0] != '-') {
			file_name = argv[i];
			break;
		}

		if (!strncmp(argv[i], "--data-dir=", 11))
			data_dir = argv[i] + 11;
		else
			die("unknown option '%s'", argv[i]);

		i++;
	}

	if (!file_name) {
		fprintf(stderr, "error: no menu-file specified\n");
		usage();
	}

	parse_xml(file_name);
	process_categories_and_populate_desktop_files();
	process_directory_files();
	print_csv_menu();

	return 0;
}
