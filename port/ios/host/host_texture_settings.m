/* The Settings app's texture pane (Settings.bundle): the size of the upscaled textures, and a switch that deletes
   them, acted on at launch and whenever the app comes back to the foreground. */
#import <UIKit/UIKit.h>
#include "host_texture_settings.h"
#include "texture_cache.h"

static NSString *cache_directory(const char *data_root)
{
	return [@(data_root) stringByAppendingPathComponent:@TEXTURE_CACHE_DIRECTORY];
}

void host_texture_settings_apply(const char *data_root)
{
	NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
	NSString *directory = cache_directory(data_root);
	uint64_t size;

	[defaults registerDefaults:@{ @"delete_upscaled_textures": @NO, @"upscaled_textures_size": @"None" }];
	if ([defaults boolForKey:@"delete_upscaled_textures"])
	{
		if (texture_cache_delete_all(directory.fileSystemRepresentation))
			NSLog(@"texture cache: could not delete everything in %@", directory);
		[defaults setBool:NO forKey:@"delete_upscaled_textures"];
	}
	size = texture_cache_size(directory.fileSystemRepresentation);
	[defaults setObject:size ? [NSByteCountFormatter stringFromByteCount:(long long)size
		countStyle:NSByteCountFormatterCountStyleFile] : @"None" forKey:@"upscaled_textures_size"];
	NSLog(@"texture cache: %llu bytes in %@", (unsigned long long)size, directory);
}

void host_texture_settings_observe(const char *data_root)
{
	NSString *root = @(data_root);

	host_texture_settings_apply(data_root);
	[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillEnterForegroundNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
			(void)note;
			host_texture_settings_apply(root.fileSystemRepresentation);
		}];
}
